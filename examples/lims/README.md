# lims — rung 3 of the [application ladder](../LADDER.md)

The ladder's large application, and **the rung that grows**: new screens,
modules and models are added here, so the ladder always has one application
whose size is what a real client faces (see [`LADDER.md`](../LADDER.md)).

**Status: built** — §1–§7 below are all implemented, with model tests under [`tests/`](tests) and its screens
under [`screens/`](screens); see [Definition of
done](#definition-of-done) for what that does and does not mean. Part A of the ERP
design (`docs/superpowers/specs/2026-11-09-lims-erp-design.md`) is built: modules, companies and sites, scoped
roles, declared authorization, scoped lists and the administration screens (decisions 21–27). A
lightweight Laboratory Information Management System:
register samples, assign analyses, capture results with real units and
detection limits on versioned forms, verify and publish, keep a regulatory
audit trail — with offline data capture in the field. The deepest test of
morph's headline claim ("exact values for financial/lab data") and of the
forms subsystem at full depth.

## Reference implementations

Three anchors, each for a different layer:

- **[SENAITE](https://github.com/senaite/senaite.core)** (Python/Plone, GPL) —
  the *domain* reference. Its code is Zope-era and not worth reading; its
  **requirements** are gold: sample → analysis request → result → verify →
  publish workflow, detection limits (`< LOD`, `> UDL`), instrument
  interfaces, and an immutable per-change audit trail built for 21 CFR Part
  11-style compliance. Mine the docs and data model, reimplement clean:
  <https://www.senaite.com/>
- **[InvenTree](https://github.com/inventree/InvenTree)** (Python/Django,
  MIT) — the *units* reference. It embeds the pint unit library end-to-end:
  parameter templates declare a base unit, users enter values in **any
  compatible unit** ("1500 mA against a template in A") and the system
  converts exactly, including in API filters; custom units are definable.
  Reproduce this flow with `morph::units::Quantity` +
  `UnitTraits<E>::relations` (entry-unit alternatives with exact ratios).
  Docs: <https://docs.inventree.org/en/latest/concepts/units/>
- **[ODK Central](https://github.com/getodk/central)** (Node, Apache-2.0) —
  the *forms + offline* reference. Its entire product is "upload a versioned
  form schema, clients render data-entry UIs from it, offline". Two features
  to reproduce:
  - versioned form definitions (XLSForm/XForms → here: versioned
    `morph::forms` schemas served by the model);
  - **offline Entities** (v2024.3+): field workers create *and update*
    shared records offline; every update carries a target **base version**;
    the server flags a conflict when the base is stale and a human resolves
    it. This is exactly morph's shared-instances + offline-queue + replay,
    with a published conflict-semantics answer to compare against. Design
    discussion: <https://forum.getodk.org/t/43272>, spec:
    <https://getodk.github.io/xforms-spec/>

## What to implement

Models: `SampleModel` keyed by sample id (shared instance — bench and office
clients attach to the same sample), `AnalysisCatalogModel` (analysis
definitions = form schemas, versioned) and `IgnitionModel` in the lab module;
`LimsAuthModel` (sign-in) and `OrgModel` (companies, sites, role assignments)
in the org module. ~~`WorksheetModel`~~ — **struck
after the rest of the rung was built; see resolved decision 19 for the
reasoning.**

Entities: client/project, sample, analysis definition (name, unit, entry
units, decimal places, specification range, LOD/UDL), analysis result,
verification record, audit entry.

Build order:

1. Analysis catalog: define an analysis with unit, precision, and spec range
   → the served JSON Schema *is* the result-entry form (`x-decimalPlaces`,
   `ExtUnits`, `x-unitAlternatives`, bounds).
2. Sample registration + lifecycle state machine
   (registered → received → in-progress → to-be-verified → published), each
   transition a guarded, journaled action.
3. **Result entry with units**: `Quantity<Unit>` fields; entry-unit
   conversion (mg/L ↔ µg/L exact); empty-Quantity = "not measured";
   detection limits as typed values. **Resolved by the round-5 review — the
   forms palette has no sum types (closed by design)**: `ResultValue =
   quantity | belowLOD | aboveUDL` is implemented as the *multi-field
   encoding* (a `Quantity` plus a qualifier `Choice`) glued by
   `mutuallyExclusive`/`exactlyOneOf` `x-rules`; the rung proves that
   encoding round-trips distinguishably through wire, journal, and offline
   payloads (three "no number" meanings — D-test in the review). Native
   sum types go on the framework-gap ledger, not this rung's critical path.
4. **Schema versioning**: editing an analysis definition creates version
   N+1; old results stay bound to their version; clients render the version
   the result was captured with (ODK's form-version model). **Scope
   correction (round 5)**: serving stored v-N schema text renders fine (the
   client machinery is data-driven), but **validation, `x-rules`, and
   precision reconciliation always run against the *current compiled*
   struct** — "bound to their version" holds for rendering only; validating
   a v-N payload under v-N rules is a named framework gap. The
   render-v1/validate-v2 skew test (review D4) is mandatory and needs no
   socket.
5. Conditional form logic: fields required/visible depending on other
   fields (e.g. dilution factor only when diluted).
   `requiredWhen`/`visibleWhen`/`readonlyWhen` with single-node conditions
   exist and are enforced client- and server-side. The compound conditions
   `andOf`/`orOf`/`notOf` exist too, are documented in `docs/spec/forms/forms.md` ("Compound conditions"), and
   are usable both nested in a `when` clause and directly as a top-level
   rule. What remains true: the vocabulary is still closed (no application
   lambdas), a hidden field's draft value still travels (decide
   clear-on-hide), and comparison rules are vacuously true on unengaged
   operands while `equals` is false — test the parity suite on *served*
   schema data including a fail-closed unknown rule kind (review D8).
6. Verification + audit: four-eyes verify step gated by `IAuthorizer` role;
   the full audit trail rendered from the journal (SENAITE's immutable
   snapshot requirement).
7. **Offline field capture** — the rung's centerpiece: a WASM/desktop client
   takes samples in the field, disconnected; results queue in
   `SqliteOfflineQueue`; each queued update carries the sample's **base
   version**; on reconnect, replay detects stale bases server-side and flags
   conflicts for human resolution instead of silently merging (the ODK
   answer, implemented on morph primitives).

## morph subsystems exercised

Unit algebra + exact conversion end-to-end; runtime schema-driven forms at
their hardest (tagged unions, conditionals, versioning); shared sample
instances; offline queue with explicit conflict semantics; role-gated
transitions; journal as regulatory audit.

## Expected strain points

- Tagged-union result values and cross-field conditional logic are beyond
  plain JSON Schema — this rung maps the exact edge of `morph::forms`.
  Wire-level corollary: **three distinct "no number" meanings** (empty
  `Quantity`, `belowLOD`, `aboveUDL`) must round-trip distinguishably
  through glaze *and* through the offline queue's opaque payloads.
- Schema versioning: morph serves schemas from compiled C++ types; versioned
  catalogs mean schemas become *data*. Bridges toward runtime custom
  fields (`tests/test_extension_bag_spike.cpp`).
- **Journal payload evolution — this rung owns the ladder's answer
  [framework gap]**: replay decodes stored payloads with the *current*
  action structs; rename or retype a field and old entries decode
  leniently, silently dropping data — the "reconstructible from the journal
  alone" DoD is then false. Versioned analyses make this unavoidable:
  per-entry schema/app-version pinning plus a migration story (the journal
  format's `v` covers the line format only).
- **Stale-schema submission**: schema `required`/bounds are client-side
  only — the server runs whatever payload arrives. A v-N payload against a
  v-N+1 server (narrowed spec range) must be accepted-under-old-rules,
  rejected, or migrated — pick one and prove it. The *journal* half of real
  binary skew is now proven by two separately compiled binaries
  (`tests/compile_checks/journal_skew_probe.cpp`): an old build records, a
  new build replays, and a renamed field throws `SchemaMismatchError` rather
  than decoding a lab result to a default. An additive field throws there
  too — `replay()`'s gate is fingerprint equality, not compatibility — and
  is admitted by a pass-through migration. This rung's shipped code never
  calls `journal::replay()` and registers no `PayloadMigrationRegistry`, so the
  fields an action gains (`RegisterSample::siteId`, `HeaderSection::siteId`)
  need no migration here: entries recorded afterwards carry the new
  fingerprint, and only a deployment that replays an older journal registers a
  pass-through migration for the action. The *wire* half (an old
  `MORPH_CLIENT_ONLY` client against a new server) stays open: nothing
  mechanically enforces the action-evolution policy on that path: there is no
  per-action `hello` fingerprint to assert on.
- **Self-conflict in the offline chain**: one field client editing the same
  sample twice offline — the second queued update's base version must
  reference the first *queued* update, not the server state, or replay
  flags the client's own second edit as a conflict (ODK hit exactly this).
- **Precision through unit relations — the rule exists; test it, don't
  redesign it** (round-5 correction): conversion carries the dp tag through
  unchanged, the renderer always submits in the canonical unit at the
  schema's `x-decimalPlaces`, and alternative-unit display rounds half-up.
  What to test instead: (a) **retag-vs-round** — settled upstream:
  `reconcileDeclaredPrecision` re-rounds rather than
  retags; this rung keeps its own stricter rejection because the governing
  precision is the analysis version's runtime value, not the compile-time
  declared one (review D1, §7 below); (b) `x-unitAlternatives` lists
  **direct relation edges only**, so InvenTree-style "enter in any
  compatible unit" needs a deliberately complete relations array; chained
  ratios are not cross-checked; (c)
  `std::optional<Quantity<U>>` silently loses all unit annotations — use
  empty `Quantity`/`optionalFields`, and lint for the optional spelling.
- **Empty-principal audit entries**: the authorize/authenticate TOCTOU can
  dispatch with a cleared principal; in a 21-CFR-framed audit trail that is
  disqualifying. Deterministic test via the injectable token clock; models
  refuse empty principals on mutating actions.
- Base-version conflict detection is app logic today — evaluate whether a
  reusable morph primitive should exist.
- Offline field capture in the browser is out of scope: `SqliteOfflineQueue`
  needs a filesystem and `NetworkMonitor` a probe thread, neither of which
  the single-threaded WebAssembly client has — desktop-first.

## Chat

The ignition tests screen (`lims.ignition`) is the proof of server-hosted chat (`docs/spec/ui/chat.md`). Each test stores
as soon as it changes, so a chat's change applies at once and appears through server push. Point `LIMS_CHAT_CONFIG` at a chat configuration file on
the server to turn the chat on (`docs/spec/ui/chat.md`, "Configuration"), and open the screen in the generic client.
"Add ignition test" and then "container 100, before 299, after 223" fill in a section, and
`lims_ignition_actions.jsonl` records each change with the mark `assistant` and the words that led to it.

The same screen works with no chat: **Add ignition test** adds a section, and its fields store on **Save**, journaled
with no mark. Each test shows `((before - after) - container) / (before - container)` once its three masses are known,
worked out on the server in exact arithmetic and shown to four places, so with a container of 100, before 299 and after
223 it reads `-0.1206`.

LIMS's sample entry screen does not yet work with a chat. It edits a working copy that a separate save stores, and its
flags are set by its own buttons, so an assistant's calls change the working copy without those flags following.

## Definition of done

- The "1500 mA vs A" InvenTree flow works with exact conversion in a
  generated form.
- Offline capture demo: two field clients update the same sample offline;
  reconnect flags exactly the stale-base update as a conflict.
- Audit trail passes the SENAITE-style test: every state a sample was ever
  in is reconstructible from the journal alone — **under the payload
  evolution scheme this rung defines**, verified by replaying a journal
  recorded before a schema migration.

## Modules

lims is built from modules, each its own library with its own include directory (`morph_add_rung`'s `MODULES`):

| Module | Holds | Links |
|---|---|---|
| `org/` (shared) | the error set, the model support every module uses, sign-in, companies and sites, role assignments, and declared authorization | nothing in lims |
| `posting/` (shared) | the posting contract: `PostingRequest` and `PostingResult`, DTOs only | `org` |
| `lab/` | samples, analyses, results, verification, offline capture, ignition tests | `org` |
| `ledger/` | the chart of accounts, the monthly periods and the journal entries (balanced, unrounded, immutable once posted, corrected by reversal; postings recorded once per source key; a closed period freezes its balances; dated exchange rates; the trial balance, account balance and account ledger reports) | `org`, `posting` |
| `billing/` | billing settings, billing clients and dated price lists with per-client prices | `org`, `posting` |
| `app/` | the server edge's `LimsAuthorizer`, and the tables it reads: `declaredRequirements` and `openMutatingActions` | every module |

The server (`src/server/`), the screens (`screens/`) and the suites (`tests/`) belong to the app and link every
module. A module that includes another module's header does not compile, and neither does one that includes a shared
module it does not use (`USES` names the shared modules each module links); the `lims.module_boundary.*` tests build
such an include and pass only when the compiler refuses it, beside a control that must build.

## Resolved design decisions

Recorded as they are taken, per the [`LADDER.md`](../LADDER.md) discipline
rule. §1–§7 are all built, with model tests under [`tests/`](tests) and
UI documents under [`screens/`](screens).

### 1. Analysis identity and analysis version are separate tables (§1)

`lims_analyses` holds the identity; `lims_analysis_versions` holds
everything a revision can change (unit, precision, spec range, LOD/UDL).
Version rows are **append-only**: `ReviseAnalysis` inserts N+1 and never
updates N. A result names a *version*, never an analysis. This is what makes
"an old result stays bound to the definition it was captured under" a
property of the data rather than a convention the code has to keep
remembering.

### 2. `UnitTraits::relations` spells out ng/L → mg/L directly (§1)

`Quantity`'s `convert` composes a path through the relation graph (BFS,
`detail::conversionRatio`), but `unitAlternatives()` — the source of the
schema's `x-unitAlternatives` — reports **direct neighbours only**. Leaving
the ng/L → mg/L edge to compose out of ng/L → µg/L → mg/L would keep the C++
conversion working while the generated form silently stopped offering ng/L
as an entry unit. The redundant-looking edge is load-bearing, and
`test_result_entry.cpp` guards it.

### 3. Every lifecycle edge is its own action type (§2)

Not a single `TransitionSample{target}`. A journal entry that cannot name
the operation is not an audit record. The guard itself is a free
`constexpr` function, `isLegalTransition(from, to)`, so the whole 6×6 matrix
is exhaustively testable with no database in the picture.

Self-edges are **illegal**: absorbing a repeated `ReceiveSample` as a no-op
would journal a transition that did not happen. `Published` and `Rejected`
are terminal. `ToBeVerified` goes forward to `Published` or back to
`InProgress` — a worked sample has results, and rejecting it outright would
discard them silently.

### 4. Refused transitions are journaled; unauthenticated ones are not (§2)

"Who tried to publish an unverified sample" is precisely the question a
21 CFR Part 11-style trail exists to answer, so a rejection appends an entry
with `Outcome::Failed` and the exception text. The single exception is
`EmptyPrincipalError`, checked *before* the recording block: an attempt with
no authenticated principal must produce no entry at all, because an audit
entry naming nobody is the failure mode this README's own "Expected strain
points" calls disqualifying. Both halves are tested and both fail if the
ordering is swapped.

### 5. The base version moves with **every** content change (§2, §3)

`lims_samples.version` is bumped by each transition *and* by each captured
result, inside the same transaction as the change itself. It is the value
§7's offline updates will target; a reader that could observe a new state at
an old version would have lost the property conflict detection depends on
before that code is written.

### 6. `ResultValue` is `exactlyOneOf(value, qualifier)` (§3)

The forms palette has no sum types (closed by design). The encoding is two
fields and one rule:

```
exactlyOneOf(&CaptureConcentration::value, &CaptureConcentration::qualifier)
```

A reading engages `value` and leaves `qualifier` empty; a non-reading does
the reverse, with `qualifier` carrying `notMeasured` / `belowLOD` /
`aboveUDL`. `exactlyOneOf` makes "0.5 mg/L, and also below the detection
limit" unrepresentable rather than merely discouraged, and one declaration
drives both sides: the renderer reads it from `x-rules`, the server re-runs
the same compiled rule list because `validate()` calls `allRulesSatisfied`.

The three "no number" meanings are three distinct strings in one `Choice`,
not one "empty" flag, which is what makes the required distinguishability
mechanical. `test_result_entry.cpp` proves it pairwise-distinct through the
wire codec, through a `LogEntry` line round trip, and through an offline
queue payload.

Decoding a qualifier code is **fail-closed**: an unknown code is rejected,
never resolved to `notMeasured`. Defaulting would turn "this client speaks a
dialect we do not know" into the lab asserting it never looked — a
fabricated claim rather than a parse failure.

### 7. Over-precise readings are rejected, not retagged (§3, review D1)

`x-decimalPlaces` "enforcement" could round the value, be advisory, or reject
an over-precise submission. The framework rounds: `reconcileDeclaredPrecision`
re-rounds on its wire dispatch paths, so storage and display agree there.

A reading finer than the method supports is a claim about the instrument, so
rounding it would record a measurement the analyst never made, and storing it
unrounded would put a number in the database that no display of it ever shows.
Either way storage and display disagree, which is disqualifying in a LIMS, so
this rung rejects the payload.

The precision it rejects against is the **analysis version's**, not the
compiled `Quantity<mg_per_L, 3>`'s. That is expressed as a
`morph::forms::InstanceConstraints` rather than a
hand-written check: the same declaration that decorated the served form's
`x-decimalPlaces` reports `precisionExceeded` here, so the number the
operator's renderer honoured and the number the server enforces are one
number. The check itself is still exact and overflow-free — `Rational` keeps
`gcd(num, den) == 1`, so a value is representable at `d` decimals exactly
when `den` divides `10^d`.

### 8. The action's unit is compile-time, so the definition's unit is checked (§3)

`CaptureConcentration::value` is a `Quantity<LimsUnit::mg_per_L, 3>`; the
only way it can be wrong about its unit is if the analysis version it names
is denominated in something else. The model checks
`version.canonicalUnit == Concentration::unitMeta().id` and refuses
otherwise — storing an amps reading in the mg/L column would be undetectable
afterwards.

### 9. Re-capturing one analysis version replaces its answer (§3)

A result is the lab's current answer for one analysis on one sample; two
live rows would make "the sample's results" ambiguous. The superseded value
is not lost — the journal holds every capture, which is exactly where a
21 CFR-style trail expects to find it.

### 10. Values are version-bound; structure is not (§4, review D4)

`GetAnalysisSchema{versionId}` serves the result-entry form for one version:
the compiled `CaptureConcentration` schema with that version's own precision
and specification range written into the framework's own keys
(`x-decimalPlaces`, `x-minimum`, `x-maximum`) via
`morph::forms::InstanceConstraints`, plus its detection limits as the
app-private `x-limitOfDetection` / `x-upperDetectionLimit`. The document
carries `x-instanceConstraints: ["value"]`, naming which keys came from the
row rather than from the compiled type. Revising an analysis leaves the old
version's served form byte-identical, which is the ODK property the README
asks for.

**Not a second key.** Serving the per-version precision as
`x-versionDecimalPlaces` beside the framework's `x-decimalPlaces` -- the shape
this needs without a framework seam, since overwriting the framework's key would
advertise a promise no code kept -- is two keys for one concept, with no way for
a renderer to know which to believe, and worse than either alone. The framework's
seam is what avoids it: one declaration both decorates the schema and checks the
submitted reading, so the advertised number and the enforced one cannot drift
apart.

The form's *shape* is still compiled and therefore identical for every
version: the `required` array and the `x-rules` list come from
`CaptureConcentration`, so a version wanting an extra field or a different
rule still cannot be served without recompiling. That boundary is unchanged,
and it is what runtime custom fields run into head-on.

`GetAnalysisSchema` **refuses** a version whose canonical unit has no
compiled result-entry action, rather than serving the mg/L form for an amps
analysis.

### 11. Replay is a dispatched action; neither half of the round trip uses a framework seam (§7)

**Corrected after the backend-mode matrix was written.** The classification
logic — base-version comparison, conflict flagging, at-most-once — lives in
`SampleModel::execute(QueuedCapture)`, a registered action. The *supported*
replay path is therefore a **re-dispatch**: a reconnecting client drains its
own queue and sends each item as an ordinary action through its authenticated
`Bridge`. `test_backend_matrix.cpp` runs that across all three deployment
modes.

`SampleModel::onBackendChanged()` is also implemented, and it is what
`docs/spec/offline/offline.md` names as the seam for exactly this. It cannot
be the primary path here: `switchBackend` *posts* the call onto the model's
own strand, where `session::current()` is null, so every queued item is
refused for want of a principal. A lab reading replayed with no identified
author is what this README calls disqualifying, so failing closed is right —
but it does mean the framework's own replay seam cannot carry an
authenticated replay. `docs/spec/offline/offline.md`'s "Conflict resolution on
replay" says so. It is only visible when replay is driven through
`switchBackend` rather than by calling the hook directly: the §7 suite's own
helper calls it from a thread that has a session installed, which the framework
never does.

The write half could not use a framework seam either: there is no
enqueue-on-failure hook, and the machine that must make the decision (a
disconnected field client) has no model on it at all.
`lab/include/lims/lab/offline/field_outbox.hpp` is the app-layer answer, and it is
app-layer by design rather than by omission: `IMPLEMENTATION.md` rule 1
carries a named carve-out for this seam, and `FieldOutbox` is the shape it
points at (`docs/spec/offline/offline.md`, "Disposition: app-layer by design").

So neither end of the offline round trip goes through a framework seam. That
is the honest summary of §7's framework story.

### 12. A conflict is a returned outcome, not a thrown error (§7)

`execute(QueuedCapture)` returns `ReplayOutcome::Conflicted` rather than
throwing. Throwing would abort the replay of every later item in the queue
and lose the very flag the run exists to raise. `IMPLEMENTATION.md`'s "never
encode failure as a magic value in a result DTO" is about *failures*; a
stale-base update is a legitimate business outcome that a human must see, so
it is an enumerator of the result.

Genuine errors still throw: an envelope that does not validate, an author who
is not the authenticated principal, a sample or analysis version that does not
exist.

### 13. Replay runs as the operator who captured the reading (§7)

`QueuedCapture::capturedBy` must equal the authenticated principal, or replay
refuses with `Forbidden`. A queued reading is replayed *as* the operator who
took it and nobody else — in a 21 CFR Part 11-style trail the author of a
reading is not a field a client may assert freely. `ApplyAnyway` resolution
keeps that attribution: the stored result names the field operator, the
conflict row names the resolver, and the journal has both.

### 14. Conflict reasons are ordered most-specific-first (§7)

A sample that has left `InProgress` has also moved on in version, so both
`LifecycleClosed` and `StaleBase` are true. The flag says `LifecycleClosed`,
because "somebody submitted this for verification while you were away" is a
more actionable thing to tell a human than "your base was stale".

### 15. At-most-once is enforced by the consumer, in a durable table (§7)

`lims_replayed_ops` records every operation key this server has *decided* —
applied **or** flagged — so a redelivery is skipped rather than acted on
twice. Without it, a second delivery would bump the version again and then
start flagging the client's own later updates as stale.

This is where `docs/spec/offline/offline.md` puts the enforcement ("the queue
… never interprets, requires, or enforces uniqueness on it — enforcement is
the replay consumer's job"), and it is also the only way to be correct on all
three shipped queues, which disagree about whether they dedup at enqueue
time.

The operation key is a minted random 128-bit id, per the spec's own
recommendation. A counter was tried first and was wrong: `FieldOutbox` holds
it in memory, so a device switched off at the end of a shift restarts the
count at zero and mints keys colliding with genuinely different earlier
edits — replay would then silently *skip* a real reading. The rung's own
self-conflict test caught it.

### 16. Four eyes is two rules, enforced in two places (§6)

The verifier must **hold the role** and must **not be the analyst who
captured the reading**. Only the first is a type-level fact, so only the first
can be checked before dispatch: `lims::auth::LimsAuthorizer` does it at the
`RemoteServer` edge, and `SampleModel` re-does it on every dispatch path
(because no authorizer runs in `Local` mode at all), checking the role at the
sample's site; decision 25 declares both rules. The second is a row-level
fact `IAuthorizer` structurally cannot express — it is handed the model and
action type ids and the caller's session, never the result — so it lives only
in the model. Neither check stands in for the other, and both are tested.

Roles are **not hierarchical**: a supervisor who must also verify is granted
`Verifier` explicitly. An implicit hierarchy is how somebody ends up being the
second pair of eyes on their own work without anyone deciding they should be.

`LimsAuthorizer` derives from `SigningAuthorizer` rather than
`AllowAllAuthorizer` because a role gate needs an
authenticated principal: a non-authenticating authorizer makes `RemoteServer`
*clear* `Context::principal` before dispatch, leaving nothing to key on. Roles
come from the `org_role_assignments` table, not the token's `roles` claim — a token
lives for its expiry, so a role revoked mid-shift would keep working until it
aged out.

**The bootstrap carve-out.** A lab with no supervisor cannot appoint its
first one, so the first `AssignRole` is allowed whoever makes it. The exception
closes the instant any supervisor exists and the assignment is journaled like every
other, so it is visible in the journal rather than a back door.
`LimsAuthorizer` deliberately does *not* mirror the carve-out — duplicating a
security exception is how the two copies drift — which means the very first
assignment must be made locally, never remotely against an empty lab.

**Publishing requires verification.** A four-eyes control nothing depends on
is theatre, so `PublishSample` refuses while any captured result is
unverified.

### 17. The audit trail is reconstructed from the journal, and only the journal (§6)

`execute(GetAuditTrail)` reads the attached action log and opens no
`DataMapper` at all. That is what makes the definition of done — "every state
a sample was ever in is reconstructible from the journal alone" — a claim the
method can support rather than assert, and the test asserts it the only honest
way: it deletes every row the sample has and reconstructs anyway.

An entry this build cannot interpret becomes an `AuditStepKind::Unreadable`
step, **never a skipped one**. Skipping is the tempting implementation and the
wrong one: a trail that quietly omits what it could not read looks complete,
so nobody discovers the omission.

**What that does not cover, stated plainly.** `Unreadable` catches an unknown
action id and a result that does not parse. It cannot catch a payload that
parses into something *else* — a renamed field decodes to a default, silently,
so a trail reconstructed across a rename is confidently wrong rather than
visibly incomplete. The framework's answer to that is
`journal::replay()`, which stamps every entry with a
payload-shape fingerprint and refuses to replay across a mismatch instead of
degrading to defaults (`docs/spec/journal/journal.md`). This rung's own
reconstruction above does not go through `journal::replay()` — it decodes
each entry's stored result with `ActionTraits<X>::resultFromJson` directly —
so it still has the gap the framework itself no longer has; adopting
`journal::replay()`'s fingerprint check here is unmade work, not an unmade
decision.

### 18. Conditional logic is declared once and evaluated twice, and this rung proves it (§5)

`CaptureConcentration` gains the README's own example: a dilution factor
required, and shown, only when the preparation says the aliquot was diluted.
Both rules are declared — `requiredWhen` **and** `visibleWhen`, over the same
`equals(dilution, "diluted")` condition — because neither implies the other,
which is the framework's stated contract rather than an oversight.

**Clear-on-hide: decided as "do not".** A hidden field's draft value still
travels; the framework says so and this rung does not fight it. Instead the
*server* ignores a dilution factor whose preparation says `neat`, so an
operator who selects `diluted`, types a factor, then switches back to `neat`
gets 2.4 mg/L rather than 24 — and gets it whether or not their renderer
bothered to clear the field. Clearing client-side would have made the outcome
depend on renderer behaviour the server cannot verify.

**The factor is load-bearing.** The model multiplies the reading by it,
exactly (`Rational`, never a `double`), so a mis-declared dilution changes the
reported concentration. A conditional field nothing depends on would test the
rule vocabulary and nothing else.

`requiredWhen` can insist a field is filled in; it cannot insist the number
means anything, so the model separately refuses a factor of zero or less, and
fail-closes on an unknown preparation code exactly as it does for an unknown
qualifier code.

**The parity suite.** `test_conditional_logic.cpp` evaluates the *served*
`x-rules` JSON with a generic, data-driven evaluator — the one a client outside
morph has to write — and asserts it agrees with the compiled `allRulesSatisfied`
across all 24 points of the rule state space, with the matrix asserted to
contain both verdicts so a client that always says yes could not pass. It also
pins the two behaviours D8 names: `equals` is **not** vacuous on an unengaged
field (unlike the comparison kinds), and an unrecognised rule kind **fails
closed on evaluation** while **deferring on enforcement** — the client does not
claim the rule holds, but still submits, because a client that blocked on an
unknown rule could not talk to a newer server at all.

That evaluator is the third implementation of one closed vocabulary the
framework owns; the shared corpus is what keeps the three honest.

### 19. `WorksheetModel` is not built, and the model list is corrected (§ model list)

A worksheet, in SENAITE's sense, groups analyses drawn from *several* samples
so one analyst can run them together — twenty nitrate determinations from
twenty samples on one plate. It is a work-organisation unit, not a data one.

It is not built, and the "What to implement" list above is corrected rather
than left carrying an unfulfilled promise. The reasoning, so a later reader
can disagree with it on the merits:

**Nothing in the rung's definition of done needs it.** The three DoD items are
the InvenTree unit flow, the offline conflict demo, and the audit trail; a
worksheet appears in none of them.

**It would exercise no morph surface this rung has not already stressed.**
Every subsystem the README's own "morph subsystems exercised" list names —
unit algebra, schema-driven forms at depth, shared sample instances, the
offline queue's conflict semantics, role-gated transitions, the journal as
audit — is covered without it.

**Its one genuinely interesting property is neutralised by a decision this
rung already took.** A worksheet writes results for samples whose own
`SampleModel` instances may be live, which is exactly where the
cross-*instance* staleness hazard `examples/bank`'s README documents ("The
honest edge") lives. It does not arise here: this rung's models cache nothing
but the id they are attached to and re-read the store on every call, so a
second writer is visible immediately. Building a worksheet to demonstrate a
hazard the design already forecloses would demonstrate nothing.

**The one gap it would re-hit is already filed.** A batch assignment
(`vector<SampleAnalysis>`) is an array-of-objects field, which the forms engine
renders as an `ObjectArray`. Building one costs a model and a table and
produces no new finding.

**What would change the answer.** A worksheet becomes worth building the day
this rung needs an operation that is *atomic across several samples* — one
transaction covering rows owned by several keyed instances. That is a real
framework question (morph serialises per instance, not across instances) and
none of §2–§7 raised it. If this rung grows a feature that needs
cross-instance atomicity, the worksheet is the natural place to put it, and this decision should be
revisited then rather than treated as settled.

## The client

lims' screens are UI documents served by the framework (spec 4), authored with the C++ builders in
[`screens/lims_screens.cpp`](screens/lims_screens.cpp) and [`screens/org_screens.cpp`](screens/org_screens.cpp):

- **`lims.register`** registers a client, then a sample for it, through mutations over a private
  instance of `SampleModel`, and opens the new sample's lifecycle.
- **`lims.lifecycle`** (`sampleId`) attaches the keyed `SampleModel` instance with `OpenSample` and
  shows the sample from a `GetSample` query that each transition's reply updates. Each of the four
  transitions is a serial mutation enabled only in the state that allows it.
- **`lims.results`** (`sampleId`) captures a concentration through a `form` node over
  `CaptureConcentration`, built from the schema the registry serves, lists the results with their
  exact decimals, verifies a result, and counts the offline conflicts.
- **`lims.sample`** (`sampleId?`) edits one sample through a private working copy held by its own
  `SampleModel` instance: `LoadSample` or `NewSample` fills it, and each section (header, sampling, and
  the optional turbidity and nitrate readings) is a `form` whose submit edits the copy and nothing else.
  `SaveSample` commits it. A refused save is a reply (`SaveError`), shown as one message per reason, not
  an error. The dilution series, with each step's recovery and its limit check, is computed by the
  model with exact decimals and shown through the `SeriesGrid` and `RecoveryCurve` host components,
  whose built-in fallbacks are what a client without them shows. A "choose project" dialog picks from
  `ListProjects`, which the table engine sorts, filters and pages. The screen's identity is its sample,
  and it is dirty while the working copy holds unsaved edits.

- **`lims.org.companies`** lists the companies and sites the viewer's roles reach and creates either through its
  action's form.
- **`lims.org.access`** lists the users and role assignments the viewer's roles reach, assigns a role through
  `AssignRole`'s form (the first assignment on a lab with no supervisor needs none) and revokes one per row.

Each action declares what it changes beside its registration (`BRIDGE_REGISTER_EFFECTS` in
`lab/include/lims/lab/models/`): a transition updates its `Lims_Sample`, a capture changes `Lims_Result`, the sample-entry
actions change their instance's working copy, and `OpenSample` changes nothing. A reply refreshes the queries it
changed, so the screens declare no refreshes; `lims_bundle_loads_without_warnings` keeps it so.

A model's refusal reaches the screen as the mutation's own error. Every typed field is schema-driven:
the `x-rules` of `CaptureConcentration` run in the forms engine, not in hand-written UI code.

The screens target links morph and the rung's models only. Its tests run on the screen harness
([`screens/tests/`](screens/tests)), and the render smoke ([`screens/render_smoke/`](screens/render_smoke))
mounts every screen on Qt Quick offscreen. `morph_examples_client --app lims` runs the rung in
process over a local database (`--db`, default `lims.db`).


### 20. Model coverage is 98.92%, and measuring it found a real defect (§ coverage gate)

`codecov.yml` gains a `lims` component scoped to the model paths of the lab and org modules
(`examples/lims/{lab,org}/src/models/**` and `examples/lims/{lab,org}/include/lims/{lab,org}/models/**`) — the paths
`examples/IMPLEMENTATION.md` rule 5's bar actually names — with a 97% target,
a margin below the measured 1557/1574 = 98.92% line ceiling (449 of 484 branches).

The remaining seventeen lines are defensive guards, each unreachable through the model's own API rather than merely
untested: `alreadyDecided`'s empty-key guard (validation refuses an empty operation key first), `renderSchemaFor`'s
malformed-JSON arm (its input is `schemaJson<A>()`), `GetAnalysisVersion`'s dangling-version arm (a declared foreign key
forbids the row), the arms of `SampleModel`'s entry actions that meet a sample row deleted between two reads of it or a
draft that cannot be encoded, and the `default` arm of the capture's constraint switch, which `checkValue` has no
other kind to reach. Each turns an impossible state into a typed error instead of a dereference, so each is kept. One
line is a measurement artifact: the continuation line of the precision refusal's `throw` is reached by its test and
reported unhit. The one reachable gap is a stored entry that does not decode, which needs a corrupted row and is not
tested.

Measuring rather than assuming was worth it twice. The first measurement came
in at 93.27%, and the gap was not padding: `AnalysisCatalogModel::attachActionLog`
had no caller at all, so the catalogue's entire journaling path was
unverified — and the audit trail's `VerifyResult` branch was **dead**, because
verifications were journaled under an *empty* entity key. That was a real
defect rather than a coverage artifact: a sample's audit trail silently
omitted the second pair of eyes, which is the one thing a four-eyes record
exists to show. Fixed by rekeying the journal to the result's own sample
(same for conflict resolutions), and pinned by its own test.

### 21. lims is built from modules, and the boundary is a link boundary

Each module is a library whose include path holds its own headers and the shared modules' only, so a module that
includes another module's header fails to compile rather than drawing a lint finding. ERP modules are written by
different teams, and the property under test is whether morph lets them cooperate without knowing each other; a
boundary that only a reviewer enforces would not test it. All modules keep the `lims` namespace and every action id,
record name, table and migration they had: the split changed where code lives, not what it does, and the suites ran
unchanged apart from their include lines. `posting`, the second shared module the design names, is added with the
first module that posts.

The boundary is deliberately a header boundary and not a data boundary. Four couplings are accepted and stated here
so they are not mistaken for leaks:

- lab's schema uses org's `SiteRecord` table name for the foreign key from `lims_samples.site_id`, because a foreign
  key names its target table and org owns the table;
- the one-time role migration writes `org_role_assignments` by raw SQL from lab's migrations, because it moves lab's
  own `lims_operators` rows into org's table and Lightweight's builder cannot copy rows;
- org's `db/` headers are on every module's include path, since a module that names an org table needs its record;
- migrations from different modules are ordered by their global timestamps, so a new module's migrations must carry
  timestamps after those of every migration they depend on.

### 22. Companies and sites live in org, and every sample is at a site

A company has a base currency and a fiscal-year start; a site belongs to one company. The first org migration seeds
one company and one site, because every record that existed before companies needs one to belong to, and the lab
migration that adds `lims_samples.site_id` puts every existing sample at the seeded site.

The site is on the sample, and on nothing hanging off it: results, verifications, conflicts and the entry record reach
their site through their sample, so a second copy of the site could only disagree with the first. Clients and
analysis definitions are reference data the whole lab shares and are not at a site; billing gives clients their
company when it needs to bill them. Ignition tests are the chat's proof and sit outside the sample workflow, so they
are not at a site either; a lab that needs them per site changes this.

A registration names its site, and a missing site is a validation error, not a default: a sample registered nowhere
would be visible to nobody once lists are scoped. Sample entry refuses a save without one as `noSite`, a reply like
its other refusals.

### 23. A role is held in a scope: a company or a site

A role assignment is `(principal, role, scope)`, kept by `lims/org` in `org_role_assignments`, and made with
`OrgModel`'s `AssignRole` and `RevokeRole`. A role held at a company holds at every site of the company; a role held
at a site holds there only. Roles themselves stay non-hierarchical (decision 16): the scope is the only thing that
nests. Grants made before scopes existed were for the whole lab, so the migration moves them to the seeded company.

The bootstrap carve-out moves with the action: the first assignment on a lab with no supervisor anywhere is allowed,
and the carve-out closes as soon as any supervisor exists. Revoking a company's last supervisor assignment is refused
(decision 24).

A local client starts on a fresh database with no assignments, and every write on a sample needs a role, so
`lims::screens::prepareLocal(user)` appoints the local user (`--user`) Supervisor at the seeded company by dispatching
the ordinary bootstrap `AssignRole` as that user, through a bridge over a local backend that carries the user's
session. It applies only while the lab has no supervisor; once one exists, roles are assigned as for any lab and
`prepareLocal` leaves them alone. The appointment is a dispatched action like any other, so it appears in the audit
trail as an `AssignRole` entry whose principal is the local user, who is also the assignment's maker.

Assignments are not a sample's history, so a sample's audit trail no longer shows them. `OrgModel` is journaled by
the framework's dispatcher, which records every loggable action it runs, and `AssignRole` and `RevokeRole` are
loggable: a successful assignment is an `Outcome::Succeeded` entry naming the caller, and a refused one (a model
refusal such as `Forbidden` or `NotFound`) is an `Outcome::Failed` entry with the exception text. Only a call that
reaches the dispatcher is journaled: `OrgModel::execute` called directly records nothing, and the lists
(`Loggable::No`) are never recorded. A `GrantRole` entry an older build wrote into a sample's journal reconstructs as
an `Unreadable` step, as any entry this build cannot interpret does (decision 17).

An assignment is decided before anything is looked up. A caller who holds `Supervisor` in no scope is refused with
`Forbidden` whether or not the scope or the assignment it names exists, so it cannot probe which ids exist; a
supervisor naming a scope that does not exist gets `NotFound`.

### 24. A company's administration is seeded by its creator and is never left empty

Roles are not hierarchical (decision 16) and nothing sits above a company, so the only people who can assign roles at
a company are its own supervisors. A company created with none could never be given a role again, and a company that
loses its last one is in the same state.

`CreateCompany` therefore appoints the calling principal `Supervisor` at the new company, in the same transaction as
the company's row: either both exist or neither does. `RevokeRole` refuses to remove a company's last `Supervisor`
assignment at that company's scope, however many supervisors exist elsewhere. A supervisor at one of the company's
sites does not count: a site role cannot administer the company scope, so it would leave the company's administration
empty all the same.

Two rules look across companies. The bootstrap carve-out applies while no `Supervisor` exists anywhere, and
`RevokeRole` refuses to remove the last `Supervisor` assignment anywhere, whatever its scope: revoking
it would reopen the bootstrap carve-out to whoever acted next.

### 25. Authorization is declared per action and checked in the model (hypothesis 6, first instance)

An action's authorization is declared once, beside its registration, by the module that owns it
(`lims::Authorization<Action>`): the role it needs, the scope its payload names, an optional value limit, and an
optional separation-of-duties pair. Capture and verify are the first declared pair: verifying a reading needs the
verifier role at the sample's site (a role at the site's company holds there), and the verifier must not be the analyst
who captured it. `lims::requireAuthorized` checks a declaration inside the model, on every dispatch path; the server
edge checks only that the principal holds the declared role somewhere, from a table the app composes from every
module's declarations (`lims::app::declaredRequirements`).

`CreateCompany` declares `Supervisor` in any scope, because a new company has no scope yet, and is refused while no
supervisor exists anywhere, so the bootstrap `AssignRole` stays the first administrative act. A supervisor at only a
site may therefore create a company; that is intended, since the rule is "a Supervisor somewhere". `CreateSite`
declares `Supervisor` at the site's company.

Every write on a sample declares that the caller holds some role, whichever, at the sample's site (a role at the
site's company counts): `RegisterSample` at the site its payload names, the lifecycle transitions,
`CaptureConcentration` and `DeleteSample` at the site of the sample the handler is attached to, `QueuedCapture` at the
site of the sample it names, `ResolveConflict` at the site of the sample the conflict belongs to, and `SaveSample` at
the site its header names and, when it updates a stored sample, at the site that sample is stored at, so moving a
sample needs a role at both sites. A principal of another company is refused on all of them. `QueuedCapture` replays
as the operator who captured the reading (decision 13), so the check applies to that operator at replay time: an
operator who has lost their role at the site is refused a new operation when the queue reaches the server, whatever
role they held when they took the reading, while a redelivery of an operation already decided still answers
`Skipped`.

Every scoped lab write follows the order decision 23 gives role assignments, one rule for the mechanism: a caller who
holds no role anywhere is refused with `Forbidden` before anything is looked up; a site, sample or conflict that does
not exist is `NotFound`; a role held only at another site is `Forbidden`. `LoadSample` stays a read. The remaining
sample-entry sections and ignition tests are open, listed in `lims::app::openMutatingActions`; ignition tests belong
to no site (decision 22).

**Hypothesis 6 (authorization cannot see the payload) is confirmed for this instance.**
`IAuthorizer::authorize(ctx, modelType, actionType)` decides before the payload is decoded: its signature
(`include/morph/session/session.hpp`) takes the session context and the two ids, never the payload. The scope (the result's sample's
site), the pair (who captured the result) and any value limit are facts about rows the payload names, so they can only
be checked where the payload is decoded and the rows are readable: in the model, by `requireAuthorized`. Measured in
`tests/test_declared_authorization.cpp`: a verifier whose role is at another site of the same company is allowed by
`LimsAuthorizer` and refused by the model in `Local`, `LocalSingleThread` and `Socket` mode alike, and a verifier at
the sample's company goes through the same edge and the same model.

This is instance 1 of 2. What it shows about a seam: a declaration is data apart from `scopeOf` and `firstPerformerOf`,
which read the module's own rows, so a framework check would have to run after decoding and call back into the
module for those; and the edge needs a second, app-composed table, because it cannot reach the declarations through
the ids it is given. No framework seam is designed from one instance. Billing's create-and-approve pair in Part B,
with an approver's value limit, is instance 2: if the same hand-rolled guarantee recurs there, a framework seam is
designed against both, and otherwise the guarantee is recorded as domain logic the models own.

### 26. A list shows only what the caller's scopes reach

`ListSamples`, `ListCompanies`, `ListSites`, `ListRoleAssignments` and `ListUsers` answer only records in scopes where
the caller holds a role: a role at a company reaches the company and all its sites, a role at a site reaches that
site and, through it, its company, including the company's own role assignments, so a site's staff can see who
supervises their company. A principal with no role, and an unauthenticated call, sees nothing. A company's creator is
assigned `Supervisor` at it in the same transaction, since a company nobody holds a role in would be listed to nobody.

`ListAnalyses`, `ListProjects` and `ListIgnitionTests` list unscoped reference data and are deliberately not
filtered: an analysis, a project and an ignition test belong to no company or site, so there is no scope to filter
by, and every authenticated caller needs them to enter a sample.

Reading one record by id (`OpenSample`, `GetSample`, the per-sample lists) is not scope-filtered in Part A: those are
not list queries, and an action that changes a record is authorized by its declaration (decision 25). Which records'
*changes* reach a principal of another company is hypothesis 7, measured with billing.

**Finding: a shared module's action cannot declare effects on another module's records.** Assigning or revoking a
role changes which samples a principal sees, so `AssignRole` changes the rows `ListSamples` answers. But `AssignRole`
belongs to `org`, which cannot name lab's `SampleView`, and a second `BRIDGE_REGISTER_EFFECTS` for the same action
from `app` is refused as "declares different effects in two registrations". So a screen listing samples does not
refresh when its viewer's roles change; it refreshes on the next sample change or reload. Registering `SampleView` as
derived from the role-assignment record would give the same refresh, but the registry refuses a derived record that has
a key (`include/morph/core/effects.hpp`, the "is derived from other records but has a key" check), and `SampleView`
has one. Inferred from reading `include/morph/core/effects.hpp` (`addEffects`, the two-declarations check, and the
derived-record check); not reproduced on a screen, because no Part A screen lists samples. A second module needing the
same (billing's lines on a role change) makes it a framework gap to file against `include/morph`.

### 27. The organisation is administered from two screens

`lims.org.companies` lists the companies and sites the viewer can see and creates either; `lims.org.access` lists
the users and role assignments the viewer can see, assigns a role and revokes one. Every form is its action's schema
and every list refreshes from the declared effects of the action that changed it, so neither screen says what to
refresh. A user is a principal holding an assignment: lims keeps no user store, and whoever signs in is a principal.
A refused revoke (the last supervisor's) shows its reason in words. The screens are functional, not finished; the
production GUI rebuilds them against the org mockups.

### 28. Money is one unit whose currency is a field beside it

An amount is `lims::Amount`, a `Quantity` over the single `ErpUnit::money` unit and an exact `Rational`, and its
currency is a `CurrencyCode` field on the same record. A unit per currency would let the type refuse adding yen to
euros, but a `Quantity`'s unit is a template argument, and the currency of a price list, an invoice or a ledger line
is known only from its row. So the models check what the type cannot: amounts they combine share a currency, and an
amount fits its currency's minor unit (JPY has none). Posters round with `lims::roundToCurrency`, half away from zero
— half-up for the positive amounts they round. Totals are summed with `lims::ExactSum`, which refuses an overflow
instead of saturating, over `Rational` values rather than `Quantity` ones, because a `Quantity` records a provenance
node per addition while `MORPH_QUANTITY_PROVENANCE` is 1 (the default). The ids billing refers to (client, sample,
analysis, result) moved from lab to org, unchanged on the wire and in the journal.

### 29. An approver's roles and ceiling are set by someone else, and a limit is confidential

Separation of duties starts before an invoice: a principal who could grant themselves `InvoiceApprover` or raise
their own value limit would hold a ceiling nobody else chose. `AssignRole` and `SetValueLimit` therefore refuse a
caller who is the subject of the action, with `Forbidden`, after the role check and before any write. It is a declared
rule beside the others (`notOnSelf` and `subjectOf` in `lims::Authorization`), judged by `requireAuthorized` in the
model. The edge sees no payload, so it cannot judge it; the model's check is the authoritative one.

Two self-assignments remain by design, and neither goes through the declared rule: the bootstrap `AssignRole` on a lab
with no supervisor anywhere (decision 23), including `prepareLocal`'s appointment of the local user, and the
appointment of a company's creator as its first supervisor by `CreateCompany` (decision 24).

A value limit is one row per principal and company, kept by a unique index on `(principal, company_id)`;
`SetValueLimit` updates the row when there is one and inserts otherwise. `ListValueLimits` returns a limit only to the
supervisors of its company and to the principal it belongs to, not to every holder of a role there.

### 30. The ledger validates and never rounds, and its totals are exact or refused (hypothesis 8, first instance)

A journal entry is recorded only if it has two or more lines whose debits equal their credits in the transaction
currency and in company currency, each amount a whole number of its currency's minor units (`lims::entryProblem`).
The ledger rounds nothing: a poster converts and rounds, half-up, line by line, and adds an explicit rounding line
where that leaves a difference. A posting is recorded once per company and source key through an `IReplayLedger` over
`ledger_posting_sources`, in the entry's own transaction, so a retry answers the first entry. Entries are immutable
once posted; a correction is a posted reversal naming the original. A period is open, closing (no new drafts;
postings and posting of existing drafts continue) or closed (nothing more is recorded; each account's totals are
frozen in `ledger_period_balances`), and closes only with no draft left in it. An exchange rate is entered per
company and currency pair from an instant (a unique index keeps one rate per pair and instant); the rate on a date is
the latest one starting on or before it, and a foreign-currency posting must state exactly that rate.

The reports (`LedgerReportModel`: trial balance, account balance as of an instant, account ledger over a range) total
posted entries only, in company currency, with `ExactSum`. A closed period that lies wholly inside the range
contributes its frozen per-account totals instead of its lines; a period the range ends inside is totalled from its
lines. A total, a balance or a running balance that does not fit `int64` is `OutOfRange`.

**Hypothesis 8, first half (magnitude): refuted for the summation.** Measured by `Stress: Hypothesis 8: ...` in
`tests/test_ledger_reports.cpp`, on the `linux-everything` Release build (clang), one run:

- 10^7 lines of pseudo-random size up to 10^9 minor units each (total 4,708,027,407,079,500 minor units, inside
  `int64`) summed through `lims::ExactSum` (`checkedAdd`) in 780 ms; every `add` reported success and the total equals
  the independently accumulated `int64` sum exactly.
- Two values of 5 * 10^18 under `ExactSum`: the first `add` succeeds, the second returns `false` and leaves the total
  unchanged.
- The same two values under `operator+` answer `9223372036854775807/1` (`INT64_MAX`) with no signal to the caller: the
  hypothesis' premise about `Rational` is true, so exactness depends on every lims total using `ExactSum`, which the
  reports and the ledger do and which nothing in morph enforces.
- A trial balance over two such postings throws `OutOfRange` instead of answering a number, both when one account's
  total does not fit and when every account fits and the company's total does not; the same holds for an account
  balance and for an account ledger's running balance.

The design's hypothesis-8 condition is met for two things: the summation step (the 10^7-line `ExactSum` case above) and
the refusal of an over-range total through the real reports. The 10^7 in-memory loop alone would also pass with plain
`operator+`, because its totals stay near 5 * 10^15; the over-range control is what distinguishes checked from unchecked
arithmetic.

The trial balance through the database was measured separately by `Stress: Hypothesis 10: ...` (same file), on the
same build: 1,000 entries of 200 lines each (2 * 10^5 lines of up to 10^9 minor units, total 47,076,604,421,980 minor
units) posted through `LedgerModel::record`, then `TrialBalance` through `LedgerReportModel`. The totals equal the
independently accumulated sum exactly. Posting took 68,135 ms and the trial balance 216 ms. Extrapolated linearly to
10^7 lines, the trial balance is about 11 s (inferred, not measured: it assumes the report's cost per line stays
constant and that 10^7 lines fit in memory). What limits it is not the summation but that the report loads every posted
line into memory with one query per posted entry (`postedLines`); that is hypothesis 10's subject, and the design states
the 10^7-line trial balance as part of it. Posting dominates the case's time (every posting is its own durable
transaction), which is why it posts few large entries and not 10^5 small ones: 50,000 two-line entries did not finish
in 590 s. On the `gcc-debug` build (the nightly's configuration) the 10^7 summation took 2,091 ms and the database case
92 s (posting 91,275 ms, trial balance 653 ms). Not measured: other hardware.

The two stress cases carry the ctest label `stress` and a 600 s limit (the rung's other tests have 120 s; the database case
needs 92 s on Debug, too near that for a loaded runner). A local `ctest -L ladder-lims` and a direct run
of the binary include them; CI's ladder legs and its coverage leg leave them out, and `nightly-slow-checks.yml` (Debug,
every test) runs them.

**Exchange rates are facts entered by hand.** A rate for each direction of a pair is its own row, entered separately;
the opposite direction is never derived as a reciprocal. A rate is not edited: a correction is a new rate with a later
start.

**One transaction currency per entry.** An entry carries one transaction currency, and balances in it and in company
currency. The design allows an entry to "sum to zero per transaction currency", which admits one entry mixing
several; lims's posters each post one document in one currency (an invoice in its client's, a receipt in its
supplier's), and a single currency per entry keeps the rate one fact of the entry rather than one per line, so
`PostingRequest` carries one `currency` and one `rate` and `entryProblem` checks that one. A document in several
currencies would post one entry per currency; nothing in Parts B and C produces one.

### 31. A price is read from the list in force, and a new list replaces the old one whole

A client is billed at a company in the currency it is registered with there (`RegisterBillingClient`); lims's client
record has no company or currency, since clients are reference data the whole lab shares (decision 22). A price list
belongs to a company, has a currency and a date it applies from; the list in force on a date is the latest that
applies by then, in the client's currency. In it, the client's own price for the analysis wins over the list's. A list
in force with no price for an analysis prices nothing: an older list is never consulted, so retiring a price is
making a list without it. A company has one list per currency and start, one price per list, analysis and client
(the list's own price is stored with client 0, since a NULL would be distinct from every other NULL in a unique
index), and one registration per client and company; each is a unique index, and a concurrent insert that loses the
race to it is answered as a `Conflict`, like the lookup that precedes it. A company's billing settings (its
receivable, revenue, tax and rounding account codes, its flat tax rate from 0 to 1 with at most six decimals, how many
hours an approval may wait, 1 to 720, and the day of the month the billing run drafts invoices, 1 to 28 so that every
month has it) are a supervisor's; setting them again replaces them. The account codes are text: billing does not read
the ledger's chart, and the ledger refuses a posting that names a code it lacks. A list stays open to new prices, including one already in
force; a price is never changed (a second one for the same analysis and client is a `Conflict`), and a correction is
a new list with a later effective date. A client id is above zero wherever one is given, since 0 is the stored key of
the list's own price.

### 32. Lab's events reach billing through an outbox, morph's relay and a dispatched consumer (hypothesis 2, first instance)

`PublishSample` writes one `lab.AnalysisPublished` row per result into `lims_outbox`, in the transaction that
publishes, with the event id `lab.AnalysisPublished:<result>:<sample version>`; a unique index refuses a second row
with the same id. `lims::app::relayLabEvents` runs one `morph::journal::OutboxRelay` pass: `drainOutbox` and
`markRelayed` are lab's (`drainLabOutbox`, `markLabOutboxRelayed`, which deletes the delivered rows), and the sink is
`lims::app::BillingEventSink`, which decodes the event and dispatches `RecordBillableLine` through
`lims::app::InternalClient`. The client is a `Bridge` over `SimulatedRemoteBackend` into the server's own
`RemoteServer`, acting as `system:events` with a token the server mints from its own secret, so the line is written
inside a dispatch: authorized at the edge and by its declared rule (the service role at the site's company, and a
`system:` principal, so a person given the role still cannot send it), journaled, and fanned out to watchers. A
`system:` principal is one no sign-in yields (`LimsLogin` refuses the namespace) and holds `LimsRole::Service` at a
company through `AssignRole` like any role; the server grants itself nothing. The bridge belongs to a one-thread
executor of its own, where every call is issued and settled, so `call` may block a relay pass without blocking the
thread that delivers its answer. That owner is an `OwnerStrand` over a one-thread pool, because a `Completion`'s
owner must be serial and a bare `ThreadPoolExecutor` is not, even with one worker. Billing records each event id in
`billing_consumed_events` through an `IReplayLedger`, in the line's transaction, so a redelivery answers the line it
made; two deliveries that race past the lookup are settled by the unique index, whose loser answers the winner's
line. A result with no price in force (or a client not registered for billing at the company) is billed `Unpriced`
with no amount rather than refused, because a consumer that refuses an event stops the relay at it. The server relays
once a second, on a background thread a `QTimer` posts to.

**Hypothesis 2 is refuted for this instance.** The criterion, set before measuring, was the design's: each event
received exactly once in effect across a restart, with only wiring code in `lims/app` and no per-consumer delivery
loop or cursor. Measured in `tests/test_domain_events.cpp`: a relay that delivers and then dies before marking leaves
one line and the event still in the outbox; a fresh relay over the same database redelivers it, billing answers the
same line (same id), and the outbox is then empty. The delivery loop is `OutboxRelay`'s and the cursor is the
outbox's own rows. What `lims/app` holds for this consumer is wiring: `BillingEventSink` (decode, dispatch, 55 lines
with `relayLabEvents`), the relay's configuration, and the principal's names. The only `for` in `event_wiring.cpp` is
`entries()`'s filter over what was delivered (the `IActionLog` read API, not delivery); a search of `app/` and billing
for cursor, last id, position and offset finds nothing; billing's only delivery-related table is the replay ledger
every consumer of an at-least-once source needs. The internal client is 177 lines in `app/` (118 header, 59 source) and
is shared by every background job. With `lineOfEvent` made always empty, the redelivery is refused at the unique index
and the relay throws, so the redelivery case fails (three checks); with the lookups and the unique index both removed,
the restart no longer leaves one line (measured: the redelivery case fails on its line count). The consumer-side
idempotence code billing hand-wrote is separate from that wiring and counted apart: about 70 lines, being the
`ConsumedEvents` adapter (36), `lineOfEvent` (14), the race catch in `storeLine` (11) and the table and unique index in
the migration (8). `billing_consumed_events` duplicates `billing_lines.event_id`; a unique index on the line's own
event id would let the line be the dedup record. The verdict stands (the design requires idempotent consumers and
excludes only a delivery loop or cursor), and Part C's second consumer is to be compared against this cost: if it
copies the adapter, the lookup and the race catch, that is a framework seam to promote, a ledger morph provides over a
consumer's own table.

Not measured, and the limits of the verdict. One consumer reads this outbox. `OutboxRelay` has one sink and
`markLabOutboxRelayed` deletes a batch whole, and `BillingEventSink` throws on an event type it does not consume
(skipping it would lose it for its real consumer once it is deleted). So a second event type in `lims_outbox`, or a
second consumer of the same event, needs a routing sink that fans one drained batch out to several consumers, which is
wiring but has to be written; that is inferred from reading `OutboxRelay`, not measured, and the second instance
decides it. A consumer that refuses an event holds back every event after it: `OutboxRelay` delivers in drain order
and marks only a whole batch (measured, same file). Exactly-once here is at-least-once delivery plus a consumer that
dedups by event id; the dedup table is the consumer's, which the design requires, and is what `IReplayLedger` is for.

### 33. An invoice is one client's lines at one site, in the client's currency; the type does not know which (hypothesis 8, invoice half of instance 2)

`CreateInvoice` collects every unbilled line of a client at a site and marks them invoiced in the same transaction, so
a second draft finds none (`Conflict`). Each line is claimed by a conditional update that requires it still
Unbilled, so a draft racing another for the same line is a `Conflict` too and its invoice row is rolled back with
it. The site must be a site of the named company, and the company must have billing settings and the client be
registered for billing there. Tax is the company's flat rate on the net total, rounded
half-up once to the currency's decimals, and the gross is net plus tax; rounding each line's tax and summing would
drift from the rate on the whole. The site is the invoice's because its approval is judged there (decision 35).
Every sum, product and the tax are the checked operations (`ExactSum`, `checkedMul`, `checkedAdd`,
`roundToCurrency`): an invoice whose lines, tax or gross do not fit an `int64` is refused as `OutOfRange` and leaves
its lines unbilled, never saturated. `GetInvoice` answers `NotFound` for an invoice of a company the caller cannot
see, and `ListInvoices` lists nothing for one.

**Hypothesis 8's runtime-currency half is confirmed for invoices.** Measured in `tests/test_invoices.cpp`: with the
currency known only from the record, the amount is a `Quantity` over the one money unit, so a yen line among a euro
client's lines is refused by `draftInvoice`, not by the type, and the served schema of a JPY price's form still
advertises `x-decimalPlaces: 2`, because the declared precision is the type's. What keeps currencies apart is model
code: `draftInvoice` refuses a line in another currency and `fitsCurrency` refuses fractional yen. `Quantity` holds the
amount exactly; it cannot hold its currency. Purchase orders in a supplier's currency (Part C) are the second
instance; a form that follows a record's currency is Part D's (`morph::forms::InstanceConstraints` serves
per-instance decimals and is the existing seam to try there).

### 34. The billing run is a cursor in billing's tables, ticked through an internal client by a timer in the server (hypothesis 4, first instance)

A run belongs to a company and a calendar month (`YYYY-MM`), at most one per pair: `StartBillingRun` refuses a
second with `Conflict`, and a unique index refuses the second of two starts that raced. Starting it writes one item per
client and site with unbilled lines. Advancing it drafts one item's invoice and marks the item done in one
transaction, so an interruption anywhere leaves every item wholly done or wholly pending. Two guarantees keep an
invoice from being drafted twice, and they are separate: the claim of the invoice's lines (decision 33) is what
refuses a second invoice for the same lines, and the cursor is what gives the run its progress and lets a resume skip
the items already done instead of walking them again. An item is claimed by a conditional update that requires it still
pending, and its invoice's lines by the update decision 33 describes; an item whose claim fails stays pending and the
next advance judges it afresh, and one whose lines were all invoiced since the run started is done with no invoice.
Progress is the run's done items of its total, counted from the items. A batch is a whole number of at least one.

The schedule is one action, `RunBillingSchedule`: it starts the run of every company that is due (the day of the
month has reached its billing day and the company has no run for the month) and advances every unfinished run by
one batch. `lims::app::BillingScheduler` dispatches it through an internal client as `system:billing-schedule`; the
server ticks it at start-up, which resumes a run a restart interrupted, and every ten seconds. One batch per tick,
each tick a dispatch, is what makes progress observable: each tick's `changes<BillingRunView>` reaches a watching
clerk, and `done` moves between ticks rather than jumping to the end inside one call. The schedule acts only for
companies where its principal holds `LimsRole::Service`, so what it may do is in the role assignments.

A refusal by the domain while drafting an item (a currency mismatch, an invoice total that does not fit) rolls that
item's draft back and marks the item `failed` with the refusal's text; the item is skipped for the rest of the run,
the other items and the other companies' runs go on, and the tick succeeds, so what it committed reaches the watching
clerks. `BillingRunView` carries `failed` (a count) and `lastError` (the text of the failed item with the highest
id). A run with no pending item left ends `Finished` when every item is done and `FinishedWithFailures` when any
failed, so a failure stays visible on a run that has stopped; its failed items' lines stay unbilled and the next
month's run picks them up. Any other exception (a database failure, a server stopping) is not the item's fault: it
leaves the item pending, ends the tick with the error, which the server logs, and the next tick resumes. A run
that starts with nothing to draft is `Finished` at once. A month the server was down for entirely gets no run; its
lines go to the next month's run. The day is the UTC day, as is the month.

**Hypothesis 4 is confirmed for this instance.** Measured in `tests/test_billing_run.cpp`: the run starts on its day
and once; each tick advances one batch, and a clerk watching `Billing_Run` is told after each; a run failed after its
first invoice shows 1 of 3 done, and a fresh server and scheduler after the "restart" finish it with one invoice per
client. The dispatch is public (an internal client over decision 32's, `billing_scheduler.hpp` is 39 lines and
`billing_scheduler.cpp` 12; `main.cpp` gains a client, a scheduler and a timer, 18 lines, and no lims code outside
the tests installs a session by hand), but nothing in morph starts work on a schedule or resumes it: the trigger is
a `QTimer` in the server, the resume is a cursor table of billing's own, and the tick is billing's own action, a
scheduler per module. morph's public timers are one-shot loop timers (`IoLoop::loop().addTimer`, which a component
such as `offline::NetworkMonitor` re-arms itself) and `TimeoutScheduler` is a detail type; nothing is periodic or
resumable. The verdict rests on where the hand-rolled code lives; the tests measure that the hand-rolled guarantee
holds. Approval escalation, the design's
second candidate, is not scheduled work (decision 35), so this is the one instance Part B has.

### 35. Invoice approval is ordinary actions on the invoice's row, and escalation is its deadline (hypothesis 5; hypothesis 4 mis-framed for escalation)

`SubmitInvoice` and `ApproveInvoice` move an invoice from draft to submitted to approved; the state, who submitted and
approved it, the approval deadline and the gross in company currency are columns of its row. The deadline is the
submission time plus the company's approval hours. Before it, the approver's role is judged at the invoice's site (a
role at the company holds there too); from it, only at the company: an approver at the site no longer can, and
`ListPendingApprovals` shows the invoice to whoever could approve it now, through the same declared rule
(`authorizationFactsOf`). The design's lifecycle names no way back from submitted, so there is none. Each transition is
a conditional update that requires the state the action read, and a statement that matches no row is a `Conflict`, so
two clerks submitting one invoice, or two approvers approving it, cannot both succeed. Approving an invoice that is
not submitted is an `IllegalTransition` for everyone, asked before the rule so that it never names who created it.
A foreign-currency invoice has no value in its company's currency until the ledger's dated rate values it, so
`SubmitInvoice` refuses one. `SubmitInvoice` declares `changes<InvoiceView>` and not `updates`: it moves a row into the
approvers' filtered list, which held no entry for its key, and `updates` refetches only the entries that hold the key.
An action that moves a row into a filtered list declares `changes`; `screens/tests/test_lims_approval_inbox.cpp`
mounts an inbox showing no pending invoice, submits, and waits for the invoice to appear.

**Hypothesis 5 is refuted for this instance.** Measured in `tests/test_invoice_approval.cpp`: an invoice submitted
by one principal on one rig is approved by another a day later on a new rig, in `Local`, `LocalSingleThread` and
`Socket` mode; four eyes and the value limit hold. The lifecycle needed no state outside the row and no code shared
with lab's verification but the declared authorization (`lims::Authorization`, `requireAuthorized`,
`authorizationFactsOf`, `authorizationVerdict`). Requisition approval in Part C is the second instance.

**Hypothesis 4 is mis-framed for escalation.** The design listed approval escalation as hypothesis 4's second
instance, but escalation is not scheduled work: it is a deadline the rule reads when an approver acts or lists, so
nothing has to start at the deadline or resume after a restart. Measured in the same file with the clock before and
after the deadline. The one consequence is an effects question, not a scheduling one, measured in
`screens/tests/test_lims_approval_inbox.cpp`: a site approver's inbox, mounted before the deadline, still lists the
invoice after it, and shows the escalation only when it asks again. What the test measures is that the open inbox is
not told: it is checked after the harness advances past the deadline. The cause is inferred, not measured: no action
ran, so no effect was declared or fanned out. Hypothesis 4 therefore has one instance in this design, the billing run
(decision 34), and the design's row says so. Part C's requisition escalation is expected to take this shape.

### 36. Billing's approval pairs and the approver's value limit are declared, and checked in the model (hypothesis 6, second instance)

`Authorization<ApproveInvoice>` declares the invoice approver role in a scope its payload's invoice decides (site or,
past the deadline, company), a value limit (the invoice's gross in company currency, recorded at submission, against
the approver's limit at that company, `SetValueLimit`), and two separation-of-duties pairs: with `CreateInvoice`, and
with `SubmitInvoice`, so an invoice the billing run drafted, its creator the schedule's service principal, still
needs a second person. `lims/org/authorization/declared.hpp` holds a form for several pairs (`separations`,
`firstPerformersOf`) beside lab's single one, and `authorizationFactsOf` gathers the facts a rule is judged on so the
pending list and `requireAuthorized` judge by the same ones. The limit is compared with the invoice's gross by an exact
comparison; no sum or conversion happens in the check, and a submission that recorded no company-currency gross
refuses the approval.

**Hypothesis 6 is confirmed for its second instance.** Measured in `tests/test_invoice_approval.cpp`:
`LimsAuthorizer` allows both the invoice's creator and an approver over their limit (it sees the principal, the model
and the action), and the model refuses both, in every mode; the clerk who submitted a run-drafted invoice is refused
too. The same hand-rolled guarantee recurred: each declared action's handler calls `requireRoleSomewhere` and
`requireAuthorized` itself (32 and 28 call sites in the modules' sources), the edge's table restates every
declaration by hand (37 entries), and the coverage test lists every mutating action by hand because nothing
enumerates a model's actions publicly. Together they are the case for a framework seam.

### 37. A draft invoice is leased to its editor, and the lease lapses with the editor's instance (hypothesis 3, first instance)

`InvoiceEditModel` is one private instance per editor. `LeaseInvoice` writes a lease row — the invoice, the holder,
the instance's token, an expiry fifteen minutes after the last edit — and refuses another editor's live lease in words
naming its holder; `GetInvoiceLease` shows the holder to anyone who can see the invoice. Each edit (removing a line,
setting the client reference) requires the instance's own live lease and renews it in the same conditional update. An
expired lease is free to the next `LeaseInvoice`, so the timeout needs no timer. Taking a lease is a write conditional
on the row as it was read, and the table has a unique index on the invoice, so two editors taking one lease at once give
one success and one `Conflict`. `ReleaseInvoice` declares `changes<InvoiceLeaseView>`, so a second clerk watching
`Billing_Lease` is told when the holder lets go — measured on a second connection.

**Hypothesis 3 is refuted for this instance, through an existing hook that is not a lease hook.** Measured in
`tests/test_invoice_lease.cpp` over a real `RemoteServer`: closing the holder's connection frees the lease, and the
second clerk takes it without waiting for the timeout. The hook is the instance's destructor: `closeConnection`
erases a private instance, and `~InvoiceEditModel` deletes its lease by token. What that costs, stated plainly: the
release does database work in a destructor, on whatever thread drops the last reference (inferred from
`closeConnection`'s documentation, not measured: the server strand when a connection closes with nothing in flight,
the model strand after the action when one is). On the server strand the blocking SQLite delete, up to the busy
timeout, stalls routing for every connection, which is the cost a framework-level release hook would have to avoid. A
failure there can only be logged; a crash that kills the process releases nothing, and the
timeout is then the only lapse. And the lapse is not told: a second clerk watching `Billing_Lease` hears of a release
but not of a lapse on disconnect, because the destructor is not an action and nothing derives a change from it —
measured on the same second connection; the clerk learns of it only by asking. The lease guards the editor's own two
edits, and `SubmitInvoice`, which refuses with `Conflict` naming the holder when it finds another principal's live
lease on the draft (a submitter's own lease is no obstacle); no other `InvoiceModel` mutation consults it. That check
and the submission's write are two steps, so a lease taken between them is not refused; nothing is lost by it, because
submission writes conditionally on the gross it read (a line removed in between is a `Conflict`, not a stale approval
amount) and the editor's later edits fail their draft check. A lease outlives submission and the holder's role until
it lapses. Lot reservation in Part C is the second instance.

### 38. Posting an invoice: two commits in sequence keep each module's action; one transaction gives the ledger's up (hypothesis 1, first instance)

`PostInvoice` builds the invoice's `PostingRequest` (`lims::postingFor`: receivable debit, revenue and tax credits,
each converted at the ledger's dated rate and rounded half-up on its own, and a line to the company's rounding account
for the difference, whole yen in a yen company) and hands it to the ledger through the port the app installs
(`lims::app::installLedgerPort`). Billing then assigns the next number of the company's fiscal-year series
(`INV-<fiscal year>-<six digits>`, the fiscal year of the entry's date, named by the calendar year it begins in),
marks the invoice posted with its entry, and commits. The number is assigned in billing's commit, so no failure leaves
a hole in the series. The port reaches the ledger one of two ways, and both were measured in
`tests/test_invoice_posting.cpp`. Dispatched, the ledger's half is `RecordPosting`, the ledger's own action, sent by
the app's internal client as `system:posting`: it is authorized by its declared rule (the service role at the
company), journaled, and its `Ledger_Entry` change reaches a watching accountant, and it commits alone, so a failure
before billing's commit leaves the ledger holding an entry for an invoice that is approved, unnumbered and unposted.
In one shared transaction, the port writes the entry through billing's own mapper (`recordPostingIn`) and both halves
commit once: a failure at either fault point leaves nothing, and no hole. The server posts dispatched.

**Hypothesis 1 is confirmed, with the workaround's cost.** The database can make the two halves one fact, and lims
did, with seams that exist, which is the design's refutation condition on its letter. But only by not running the
ledger's half as the ledger's action. Measured, in the shared transaction: no `Ledger_Entry` change reached the
watching accountant (it did in the dispatched posting); the posting succeeded with the posting principal holding no
role, where the dispatched posting was refused (at the server, by the declared role table or the ledger's rule; the
test does not separate the two); and the journal holds no `LedgerModel` `RecordPosting` for it (it does for the
dispatched posting). With the ledger's half as its action, the window is real: a failure at either fault point leaves
the entry committed and the invoice approved, unnumbered and unposted, and no number is spent. What morph lacks is a
way to run two models' actions as one fact while keeping each one's authorization, effects and journal. The server
keeps the dispatched posting until that seam exists. Goods receipt in Part C is the second instance, and the seam is
designed against both.

Three costs of the dispatched posting that only measuring showed:

- **A retry must repeat the request, date included.** The ledger answers a repeated source key only for an identical
  request, and the request is dated by the clock. A retry on another day would send another date and be refused as a
  different posting. `PostInvoice` therefore asks the ledger first (`FindPosting`) for the entry under its key and, if
  there is one, sends that entry's date, and values a foreign-currency invoice at the rate on that date. The retry then
  converges on the recorded entry and numbers the invoice in the fiscal year of the entry's date, which can be the year
  before the retry's. The shared transaction needs none of this: nothing outlives a failure.
- **The ledger's transaction takes the write lock first.** SQLite turns a transaction that read and then wrote away at
  once, without waiting, when another holds the write lock, so two postings at once failed with a raw "database is
  locked". `recordPostingIn` opens with a statement that writes nothing and takes the lock, and a database that is
  still busy after the ledger's retry is a `Conflict`, as it is in billing's numbering transaction. Two clerks posting at
  once get consecutive numbers; the test holds both postings between the halves so that their numbering transactions
  overlap.
- **Posting is conditional on the state it read.** The invoice moves from approved to posted by a conditional update,
  and the series advances only from the number read, so a second posting of one invoice changes nothing and spends no
  number.

`SubmitInvoice` is a Task handler that awaits the same port's `GetRateOn` to value a foreign-currency invoice at the
ledger's rate on the submission date, so the approver's limit is judged against its worth in the company's currency; it
refuses with a `ValidationError` when no ledger is wired or no rate is valid.

### 40. An invoice change in one company reaches another company's watching clerk (hypothesis 7, first instance)

**Hypothesis 7 is confirmed for invoices, and what leaks depends on the effect the action declares.** Measured in
`tests/test_two_company_effects.cpp` over a real `RemoteServer`, with the first company's clerk watching
`Billing_Invoice` on a connection of her own and the second company's clerk watching it on another as the control:

- `CreateInvoice` declares `changes<InvoiceView>`. Company B creates an invoice; company A is sent the record without
  any key ("sent to company A: Billing_Invoice as a whole", printed by `-s`). It learns that the second company's
  invoices changed, and when.
- `ApproveInvoice` declares `updates<InvoiceView>`. Company B approves an invoice; company A is sent the record with
  the invoice's id ("sent to company A: Billing_Invoice with key 1", the same key company B's own clerk is sent).
  `Billing_Invoice` is registered without `record::privateKeys`, so nothing strips the key. Declaring
  `changes<InvoiceView>` on `ApproveInvoice` instead makes the same case fail (the approval is then sent as a whole
  record to both companies).

This is the framework's documented design and not a defect: record events are type-level, a watch is admitted and
re-authorised by whether its principal may still run some action that returns the record, and nothing in that path
sees the row. docs/spec/security.md, "What an event reveals": "Those keys are ids of rows the recipient could list
through some action returning that record, but not necessarily the same rows: a model may filter rows per principal";
`privateKeys` is the documented way to withhold them. Leaving `privateKeys` off a tenant-owned record therefore
discloses its ids across tenants whenever an action declares `updates<>` on it. The lists stay scoped by the
company in `ListInvoices`' payload and the visibility check the model runs on it (inferred from the handler; the
refetch was not measured here). Hiding
the event itself needs a row-company judgement in the fan-out, which only the model has (inferred from the fan-out's
documentation). If Part C's stock instance needs the same workaround (per-company record names, or `privateKeys` on a
tenant-owned record), a per-tenant scoping seam for record events becomes an enhancement against `include/morph`;
until then it is not filed.

## Findings raised by this rung

- **A data migration that copies rows needs raw SQL.** Lightweight's migration builder inserts literal rows only, so
  moving `lims_operators` into `org_role_assignments` is one `RawSql` `INSERT ... SELECT`
  (`lab/src/db/schema.cpp`, migration `20261109000004`), under IMPLEMENTATION.md rule 4's escape tier. Upstream: a
  search of LASTRADA-Software/Lightweight's issues for "migration insert select" finds none for this; none was filed.

- **A conditional UPDATE is the query builder, not the DataMapper.** The ledger changes an entry's state or its
  reversed mark only where the row still holds what the action read (`state` is Draft; `reversed_by_id` is NULL), and
  a statement that matches no row is a `Conflict`, so two callers acting on one read cannot both succeed.
  `DataMapper::Update` writes by primary key alone and has no WHERE of its own, so `ledger_entries.cpp` builds the
  statement with `Connection().Query(table).Update().Set(...).Where(...)` and counts the rows the cursor reports. That
  is the typed query tier, not raw SQL. The race is simulated, not run concurrently: the tests make a trigger mark
  the original between the read and the mark, which fails with the guard removed; the post guard's condition has no
  such seam, so only its zero-row handling is tested.
  Billing's `draftInvoice` claims each line the same way (`state` is Unbilled) and counts the rows.

- **A source key is bound to one posting body.** The ledger records a posting once per company and source key. A
  repeat of the same request (date, source reference, currency, rate, and every line's account, side and amounts,
  compared by value) answers the first entry and writes nothing, even after the dated rate or the period has
  changed; a repeat with a different body is a `Conflict`, because answering the first entry would tell the poster
  its changed document was booked. Two simultaneous first calls with one key record one entry: the unique index
  refuses the second row, that call rolls back and answers as a repeat.

- **SQLite foreign keys are not enforced on the lims connection.** `PRAGMA foreign_keys` answers 0 there (measured by
  a test), so the foreign keys the ledger migration declares (line to entry, line to account, entry to period) are
  declared but not enforced. What keeps a line on its own company's chart is the company filter in the ledger's
  account lookup, and a read of a line whose account is gone is refused rather than shown with an empty code.

- **`ModelKey` rejected strong id types. Closed upstream.** `BRIDGE_MODEL_KEY`
  routes the key through `keyToString`, whose concept admitted only
  `std::integral` or `std::string`, while `IMPLEMENTATION.md` rule 3 mandates
  a strong id struct for entity identity. `ModelKey` admits a strong id wrapping a raw key
  (`WrappedModelKey`) now, so no rung carries a hand-written block —
  `SampleModel` keys on `SampleId` itself, and an empty id is refused
  by `keyToString` instead of dereferenced.
- **a forms schema is a pure function of the compiled action type.**
  **Partly closed upstream.** Per-instance *values* now reach the framework's
  own keys through `morph::forms::InstanceConstraints`: the two analysis
  versions declaring 3 and 1 decimal places serve `"x-decimalPlaces":3` and
  `"x-decimalPlaces":1`, and a reading outside the served specification range
  is flagged (`ResultView::outOfSpec`) instead of being stored with nothing
  recording it. What remains is the structural half — the schema's *shape* is
  still a function of the compiled type, so a rung whose definitions are data
  needs one compiled action per unit family rather than per analysis. That is
  what runtime custom fields run into head-on.
- **a journal entry from an older build decodes leniently to defaults, with no
  signal.** **Closed at the framework level; this rung's own audit
  reconstruction has not adopted it.** `journal::replay()` now stamps every
  entry with `morph::model::payloadFingerprint<Action>()` and throws
  `SchemaMismatchError` when a recorded entry's shape disagrees with what the
  running build decodes it as, instead of silently degrading to defaults (see
  `docs/spec/journal/journal.md`, "Payload schema fingerprint"). This rung's
  own `SampleModel`/`ResultModel` audit-trail reconstruction
  (`sample_model.cpp`) calls `ActionTraits<X>::resultFromJson` directly rather
  than `journal::replay()`, so it still has no fingerprint check and a renamed
  field still decodes to its default with no signal — the finding entry below,
  §17 in "What that does not cover, stated plainly," is accordingly still
  live for this rung specifically, even though the framework question it
  raised is answered. (Filed; closed upstream.)
- **A schema's `required` array can silently contradict its own `x-rules`.**
  **Closed.** `schemaJson`'s required-by-default rule put both
  `value` and `qualifier` in `required` while the `exactlyOneOf` entry beside
  them said at most one may be engaged — an unsatisfiable form. `schemaJson<A>()`
  now rejects exactly this at generation, throwing
  `morph::forms::UnsatisfiableFormError` from `detail::rejectUnsatisfiableRules`
  (called from `mergeSchemaExtras`, the one place both halves are in hand), so
  the contradiction is a loud generation-time error instead of a silent one —
  documented in `docs/spec/forms/forms.md`, "Unsatisfiable declarations", whose
  worked example is this rung's own `CaptureConcentration`. The sanctioned
  escape (`optionalFields`) is still what makes the rule the only gate on the
  pair; `CaptureConcentration`'s own doc comment
  (`lab/include/lims/lab/dto/result_dto.hpp`) explains why it is still needed even
  though omitting it is now a loud error rather than a silent one. Guarded
  here by a test asserting `required` is exactly `["analysisVersionId"]`.
- **the three shipped `IOfflineQueue`s disagree about repeated idempotency
  keys.** **Closed.** `InMemoryOfflineQueue` admits the duplicate;
  `FileOfflineQueue` and `SqliteOfflineQueue` dedup — a deliberate
  strengthening the base contract permits but did not document. The
  interface header (`include/morph/offline/offline_queue.hpp`) now states the
  hit semantics explicitly: a dedup hit is first-write-wins with silent
  payload loss, matching what `docs/spec/offline/offline.md` already said.
- **the offline write path has no model-side seam.** Rule 1 says all domain
  logic lives in models; the offline spec says enqueue-on-failure is the
  application's job at the dispatch site; and a disconnected field client has
  no model to put it in anyway. `FieldOutbox` is this rung's app-layer answer,
  and it carries a real invariant (a client's own successive offline edits must
  chain), not glue. **Dispositioned as app-layer by design:** rule 1 names the
  carve-out and `docs/spec/offline/offline.md` records the reasoning and the
  boundary; a framework primitive is reconsidered when a third rung grows its
  own enqueue path.
- **`MORPH_BUILD_OFFLINE_SQLITE=ON` breaks the build on macOS with a non-Apple
  clang.** `FindSQLite3` resolves the SDK's whole `/usr/include`, which is then
  injected as `-isystem` ahead of libc++'s own headers. The repo's own
  `morph_offline_sqlite_tests` target fails identically, which is why the
  durable queue had never been built here before.
- **`x-rules` has one client-side evaluator and no shared corpus.** **Closed.**
  A shared corpus pins the compiled evaluator
  (`morph::forms::allRulesSatisfied`) to the forms engine's: one checked-in
  file, `tests/data/rule_corpus.json`, is driven through both —
  `tests/test_forms_rule_corpus.cpp` on the compiled side,
  `tests/test_forms_engine_corpora.cpp` on the client side — so a rule kind
  added to one evaluator and not the other fails a shared test instead of
  shipping silently divergent. A client outside morph still has to
  reimplement the vocabulary; the corpus is what keeps that reimplementation
  honest against the compiled evaluator's behavior.
- **a ladder test whose name contains a semicolon never gets its
  `ladder-<rung>` label.** `morph_add_rung`'s re-labelling step iterates
  `IN LISTS`, which splits the name, so `set_tests_properties` applies to
  nothing and nothing warns. Found here the hard way: `ctest -L ladder-lims`
  reported 85 cases while the binary reported 87. Two lims cases were renamed;
  12 pre-existing ones repo-wide are still affected. (Filed.)
- **a `Quantity`'s exact decimal can only be rendered with its unit appended.**
  **Closed.** `toDecimalString(quantity)` (`include/morph/util/quantity.hpp`)
  is now the public decimal-only renderer — the exact numeric half of
  `toString`, no unit suffix, `"N/A"` for an empty quantity — so a view that
  places the number and the unit separately (a table with the unit in its
  column header, a right-aligned suffix) asks for the two halves independently
  instead of concatenating them and chopping the suffix back off.
- **`Model::onBackendChanged()` runs with no session.** **Closed as a
  documentation correction, not a code gap.** `LocalBackend` never consults an
  authorizer at all, so no version of this hook could ever carry a *verified*
  principal — a session-plumbing fix was the wrong ask. `switchBackend` still
  posts the hook onto the model's strand where `session::current()` is null,
  and `docs/spec/offline/offline.md`'s "Conflict resolution on replay" section
  now says so explicitly instead of implying the seam can authenticate a
  replay. Found by driving replay through `switchBackend` rather than calling
  the hook directly — the §7 suite's own helper had been supplying a session
  the framework never does.
- **Models cannot self-journal without the registry/dispatcher path.**
  `IModelHolder::recordIfAttached` fires only for holder-constructed models,
  so a plain-constructed one — the only kind a unit test has, and what
  `IMPLEMENTATION.md` rule 5 requires the audit trail to be tested through —
  records nothing, so a model that journals carries its own
  `attachActionLog`/`logAction` pair; this rung shares one copy between its
  two models (`lab/include/lims/lab/core/self_journal.hpp`). Not
  filed as a separate finding pending a decision on whether "models do not
  journal, dispatchers do" is the intended contract — but if it is, the
  contract makes the "reconstructible from the journal alone" DoD untestable
  at the model level, which is worth stating in
  `docs/spec/journal/journal.md` either way.
- **One compiled action type per unit family, not per analysis.**
  `Quantity`'s unit and precision are template parameters, so a catalogue
  whose analyses are *data* cannot produce a result-entry action per
  analysis. `CaptureConcentration` covers every mg/L-denominated analysis
  and a second family (amps, temperature, pH) needs a second action type.
  This is the concrete shape of the README's "schemas become data" strain
  point and it bounds §4 as well.
