# lims — rung 6 of the [application ladder](../LADDER.md)

**Status: design annex** ([round-7 program decision](../LADDER.md)) — this
README is the deliverable; construction is a post-rung-4 decision, and the
rung's sharpest content (forms conformance D1–D8, journal payload
evolution) runs earlier as no-app spikes. A lightweight Laboratory
Information Management System:
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
definitions = form schemas, versioned). ~~`WorksheetModel`~~ — **struck
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
   exist and are enforced client- and server-side. **Correction (rung 6,
   verified against the shipped headers):** round 5's "there are no
   `and`/`or`/`not` combinators" is **out of date** — `andOf`/`orOf`/`notOf`
   landed in commit 332f82c ("forms+qml: and/or/not rule conditions", #78),
   are documented in `docs/spec/forms/forms.md` ("Compound conditions"), and
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
  catalogs mean schemas become *data*. Bridges toward rung 7's runtime
  custom fields.
- **Journal payload evolution — this rung owns the ladder's answer
  [framework gap]**: replay decodes stored payloads with the *current*
  action structs; rename or retype a field and old entries decode
  leniently, silently dropping data — the "reconstructible from the journal
  alone" DoD is then false. Versioned analyses make this unavoidable:
  per-entry schema/app-version pinning plus a migration story (the journal
  format's `v` covers the line format only). Rungs 5 and 7 reuse whatever
  is decided here.
- **Stale-schema submission**: schema `required`/bounds are client-side
  only — the server runs whatever payload arrives. A v-N payload against a
  v-N+1 server (narrowed spec range) must be accepted-under-old-rules,
  rejected, or migrated — pick one and prove it. The *journal* half of real
  binary skew is now proven by two separately compiled binaries
  (`tests/compile_checks/journal_skew_probe.cpp`): an old build records, a
  new build replays, and a renamed field throws `SchemaMismatchError` rather
  than decoding a lab result to a default. An additive field throws there
  too — `replay()`'s gate is fingerprint equality, not compatibility — and
  is admitted by a pass-through migration. The *wire* half (an old
  `MORPH_CLIENT_ONLY` client against a new server) stays open: nothing
  mechanically enforces the action-evolution policy on that path until
  issue #207's per-action `hello` fingerprint exists to assert on.
- **Self-conflict in the offline chain**: one field client editing the same
  sample twice offline — the second queued update's base version must
  reference the first *queued* update, not the server state, or replay
  flags the client's own second edit as a conflict (ODK hit exactly this).
- **Precision through unit relations — the rule exists; test it, don't
  redesign it** (round-5 correction): conversion carries the dp tag through
  unchanged, the renderer always submits in the canonical unit at the
  schema's `x-decimalPlaces`, and alternative-unit display rounds half-up.
  What to test instead: (a) **retag-vs-round** — resolved upstream in
  issue #159, which made `reconcileDeclaredPrecision` re-round rather than
  retag; this rung keeps its own stricter rejection because the governing
  precision is the analysis version's runtime value, not the compile-time
  declared one (review D1, §7 below); (b) `x-unitAlternatives` lists
  **direct relation edges only**, so InvenTree-style "enter in any
  compatible unit" needs a deliberately complete relations array; chained
  ratios are not cross-checked; (c) the shipped QML converter silently
  clears input above a 1e12 divisor — exactly the fine-ratio range of
  trace-concentration relations (ng/L↔mg/L); (d)
  `std::optional<Quantity<U>>` silently loses all unit annotations — use
  empty `Quantity`/`optionalFields`, and lint for the optional spelling.
- **Empty-principal audit entries**: the authorize/authenticate TOCTOU can
  dispatch with a cleared principal; in a 21-CFR-framed audit trail that is
  disqualifying. Deterministic test via the injectable token clock; models
  refuse empty principals on mutating actions.
- Base-version conflict detection is app logic today — evaluate whether a
  reusable morph primitive should exist.
- Offline field capture in the browser inherits kanban's WASM-offline scope
  limits ([`../kanban/README.md`](../kanban/README.md)) — desktop-first.

## Definition of done

- The "1500 mA vs A" InvenTree flow works with exact conversion in a
  generated form.
- Offline capture demo: two field clients update the same sample offline;
  reconnect flags exactly the stale-base update as a conflict.
- Audit trail passes the SENAITE-style test: every state a sample was ever
  in is reconstructible from the journal alone — **under the payload
  evolution scheme this rung defines**, verified by replaying a journal
  recorded before a schema migration.

## Resolved design decisions

Recorded as they are taken, per the [`LADDER.md`](../LADDER.md) discipline
rule. §1–§3 are built; §4–§7 are not yet.

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

Upstream issue #159 asked whether `x-decimalPlaces` "enforcement" should
round the value, be redocumented as advisory, or reject an over-precise
submission. The framework took the first option: `reconcileDeclaredPrecision`
now re-rounds on its wire dispatch paths, so storage and display agree there.

A reading finer than the method supports is a claim about the instrument, so
rounding it would record a measurement the analyst never made, and storing it
unrounded would put a number in the database that no display of it ever shows.
Either way storage and display disagree, which is disqualifying in a LIMS, so
this rung rejects the payload.

The precision it rejects against is the **analysis version's**, not the
compiled `Quantity<mg_per_L, 3>`'s. That is now expressed as a
`morph::forms::InstanceConstraints` (upstream issue #164) rather than a
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

**Corrected by upstream issue #164.** This rung originally served the
per-version precision as a *second* key, `x-versionDecimalPlaces`, beside the
framework's `x-decimalPlaces`, because overwriting the framework's key would
have advertised a promise no code kept. Two keys for one concept, with no way
for a renderer to know which to believe, was worse than either alone. The
framework now has a seam for it: one declaration both decorates the schema and
checks the submitted reading, so the advertised number and the enforced one
cannot drift apart. `x-versionDecimalPlaces` is gone.

The form's *shape* is still compiled and therefore identical for every
version: the `required` array and the `x-rules` list come from
`CaptureConcentration`, so a version wanting an extra field or a different
rule still cannot be served without recompiling. That boundary is unchanged,
and it is what rung 7's runtime custom fields run into head-on.

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
authenticated replay. That is morph#201, and it was found only by
driving replay through `switchBackend` instead of calling the hook directly;
the §7 suite's own helper calls it from a thread that has a session
installed, which the framework never does.

The write half could not use a framework seam either: there is no
enqueue-on-failure hook, and the machine that must make the decision (a
disconnected field client) has no model on it at all.
`include/lims/offline/field_outbox.hpp` is the app-layer answer, and
morph#197 is the finding — now dispositioned: `IMPLEMENTATION.md` rule 1
carries a named carve-out for this seam, and `FieldOutbox` is the shape it
points at (`docs/spec/offline/offline.md`, "Disposition: app-layer by design").

So neither end of the offline round trip goes through a framework seam. That
is the honest summary of §7's framework story.

### 12. A conflict is a returned outcome, not a thrown error (§7)

`execute(QueuedCapture)` returns `ReplayOutcome::Conflicted` rather than
throwing. Throwing would abort the replay of every later item in the queue
and lose the very flag the run exists to raise. `IMPLEMENTATION.md`'s "never
encode failure as a magic value in a result DTO" is about *failures*; a
stale-base update is a legitimate business outcome that a human must see, and
the two-enumerator result enum is the same shape as `polls::Restored`.

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
three shipped queues, which disagree about whether they dedup at enqueue time
(morph#175).

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
`RemoteServer` edge, and `SampleModel` re-does it on every dispatch path,
because no authorizer runs in `Local` mode at all. The second is a row-level
fact `IAuthorizer` structurally cannot express — it is handed the model and
action type ids and the caller's session, never the result — so it lives only
in the model. Neither check stands in for the other, and both are tested.

Roles are **not hierarchical**: a supervisor who must also verify is granted
`Verifier` explicitly. An implicit hierarchy is how somebody ends up being the
second pair of eyes on their own work without anyone deciding they should be.

`LimsAuthorizer` derives from `SigningAuthorizer` rather than
`AllowAllAuthorizer` (polls' choice) because a role gate needs an
authenticated principal: a non-authenticating authorizer makes `RemoteServer`
*clear* `Context::principal` before dispatch, leaving nothing to key on. Roles
come from the `lims_operators` table, not the token's `roles` claim — a token
lives for its expiry, so a role revoked mid-shift would keep working until it
aged out.

**The bootstrap carve-out.** A lab with no supervisor cannot appoint its
first one, so the first `GrantRole` is allowed whoever makes it. The exception
closes the instant any supervisor exists and the grant is journaled like every
other, so it is visible in the trail rather than a back door.
`LimsAuthorizer` deliberately does *not* mirror the carve-out — duplicating a
security exception is how the two copies drift — which means the very first
grant must be made locally, never remotely against an empty lab.

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
visibly incomplete. That is morph#174, and this rung does **not**
close it: the README names this rung the owner of the ladder's payload-evolution
answer, and shipping a half-considered scheme that rungs 5 and 7 must then
live with would be worse than filing the diagnosis with the options laid out.

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
`x-rules` JSON with a generic, data-driven evaluator — the one a non-QML client
has to write — and asserts it agrees with the compiled `allRulesSatisfied`
across all 24 points of the rule state space, with the matrix asserted to
contain both verdicts so a client that always says yes could not pass. It also
pins the two behaviours D8 names: `equals` is **not** vacuous on an unengaged
field (unlike the comparison kinds), and an unrecognised rule kind **fails
closed on evaluation** while **deferring on enforcement** — the client does not
claim the rule holds, but still submits, because a client that blocked on an
unknown rule could not talk to a newer server at all.

That evaluator is the third implementation of one closed vocabulary the
framework owns, which is morph#176.

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
(`vector<SampleAnalysis>`) is an array-of-objects field, which `DynamicForm`'s
array control does not render — a gap `examples/polls` hit and recorded first.
Re-confirming it costs a model and a table and produces no new finding.

**What would change the answer.** A worksheet becomes worth building the day
this rung needs an operation that is *atomic across several samples* — one
transaction covering rows owned by several keyed instances. That is a real
framework question (morph serialises per instance, not across instances) and
none of §2–§7 raised it. If a rung 7 or 8 needs cross-instance atomicity, the
worksheet is the natural place to put it, and this decision should be
revisited then rather than treated as settled.

## The client

Two surfaces, one shell (`gui/qml/Main.qml`): the sample lifecycle and result
entry, side by side as tabs rather than stacked, because both act on the
*same* attached sample at once — a bench operator captures a reading while the
office watches the state move. Attaching the lifecycle surface points the
result surface at the same sample, and both handlers are `AllowShared` over a
keyed model, so they land on one instance. That is the shared-instance design
made visible rather than asserted.

`gui_lib/` holds two presenter/QML-bridge pairs (`SamplePresenter`/
`SampleBridge`, `ResultPresenter`/`ResultBridge`) plus one shared conversion
header. Presenters link `Qt6::Core` only and instantiate under a plain
`QCoreApplication` (presenter rule 1); they track completions through the
common `Presenter` base so tests wait on `busy()` rather than sleeping (rule
3); QML is bindings-only (rule 6); and the offscreen engine-load smoke test
is registered in ctest.

### Every typed field is schema-driven; nothing is hand-built

`examples/IMPLEMENTATION.md` rule 2 permits no hand-built input widgets, and
this rung has none. The rule that decides which actions get a form:

> **A field a person types is rendered from the served schema. A value the
> model already supplied is a typed call.**

So `RegisterClient`, `RegisterSample`, `RejectSample`, `ReturnForRework`,
`CaptureConcentration` and `ResolveConflict` are all rendered through the
shipped `DynamicForm` from `gui_lib/lims_schemas.hpp`'s document. The
zero-field transitions (`ReceiveSample`, `StartWork`,
`SubmitForVerification`, `PublishSample`) are plain buttons, because an
action with no fields has no form to generate. `VerifyResult` and
`OpenSample` are typed calls: their one field is an id the table or spinbox
already holds, and asking somebody to retype an id they are looking at would
be worse, not more conformant.

The capture form is the one that earns its keep. Rendering
`schemaJson<CaptureConcentration>()` through the shipped renderer is what puts
this rung's own cross-field rules in front of the **framework's** evaluator
rather than only the one `test_conditional_logic.cpp` writes:
`exactlyOneOf(value, qualifier)` becomes the submit gate that makes "a number
*and* a below-LOD flag" unsubmittable; the `requiredWhen`/`visibleWhen` pair
makes the dilution factor appear and become required exactly when the
preparation says diluted; and `x-decimalPlaces`/`x-unitAlternatives` drive the
entry-unit machinery. None of that is written in QML.

Every `DynamicForm` here is left **unbound** (`controller: null`) and
submitted by an explicit button, following bookmarks' and pastebin's
precedent: a bound form auto-submits the moment its required fields are
engaged, which for a lab reading would file a result mid-keystroke.

### Two channels carry a refusal, and both are bound

A refusal is this rung's product, not its exception path — an over-precise
reading (decision 7), an `exactlyOneOf` violation (decision 6), a four-eyes
refusal (decision 16), an unknown qualifier or dilution code (decision 18) and
a rejected conflict resolution are each a statement about the measurement that
the analyst has to read. So both surfaces bind both channels the bridges have:

- **`failed` / `lastError`** carries the *typed* invokables' errors —
  `openSample`, `refresh`, the zero-field transitions, `verifyResult`. It is
  bound as the red label in `Main.qml` and at the foot of `ResultEntryView.qml`.
- **`replyReceived(actionType, ok, payload)`** carries every *schema-driven*
  form's outcome, both ways, with the model's own `what()` as the payload when
  `ok` is false. `submitIfValid` is the only path those six forms have and it
  never routes through `failed`, so each view handles `replyReceived` in a
  `Connections` block — the same shape bookmarks, pastebin and polls use — and
  clears a form only once the submission was actually accepted.

`test_lims_qml_surface.cpp` asserts the second half rather than assuming it:
one case runs the QML-surface audit unexempted and requires that no finding
names `replyReceived`, so deleting either `Connections` block fails the build's
tests instead of quietly emptying the screen.

### Two handlers on the lifecycle surface, and why

`SampleModel` is keyed, so the handler every attached action runs on is
`AllowShared`. But an `AllowShared` handler is **not bound until it attaches
to a key**, and `RegisterClient`/`RegisterSample` carry none — dispatching
either on it fails with "handler not bound". This was confirmed empirically
here before `SamplePresenter` grew a second, plain handler for exactly those
two actions; `polls::gui::PollPresenter` reached the same conclusion for
`CreatePoll`. `RegisterSample` needs no such help even though it too arrives
before any key exists: it is result-keyed, so `BridgeHandler::execute`'s
`ResultKeyed` branch runs it on an anonymous instance and promotes that
instance to the id the result names before the completion resolves — one
dispatch, and the shared handler is attached when it returns.

That asymmetry is why the two registration invokables survive the surface trim
below while the other typed calls do not. `submitIfValid` routes both
registration actions to the *plain* handler, so through the form path
`RegisterClient` never sets the `clientId` property and `RegisterSample` never
leaves the shared handler attached — the invokables are the only dispatch that
does either, which is exactly what their exemptions in
`test_lims_qml_surface.cpp` say.

### One dispatch path per action (morph#287)

Both bridges published a typed invokable *and* a schema-driven form for the
same action: `registerClient`/`registerSample`/`rejectSample`/
`returnForRework` beside `submitIfValid("RegisterClient")` and its three
siblings, and `captureReading`/`captureQualifier`/`resolveConflict` beside
`submitIfValid("CaptureConcentration")` and `submitIfValid("ResolveConflict")`.
No QML called the typed half. Two paths to one action is two places for the
behaviour to differ, and here they already did: `captureReading` took the
reading as a `double` and converted it with `Concentration::fromDouble` at the
field's declared precision, so it *rounded* an over-precise reading that the
form path submits exactly and the model refuses (decision 7) — and it was the
only `double` on this rung's QML surface, against the convention
`gui_lib/lims_qml_conversions.hpp` states for a `Quantity`.

So the redundant half is deleted, together with the outcome signals only it
emitted (`resultCaptured`, `conflictResolved`), the `sampleAttached` signal
whose state `Main.qml` re-reads through `refreshResults()` instead, and the
`bound` relay neither view handles. `rejectSample` and `returnForRework` are
gone from `SamplePresenter` too, since nothing else called them. What survives
is one path per action, and a QML-surface exemption list of two entries, each
naming a mechanism rather than a backlog.

### What the smoke test does not prove, and what covers it instead

The offscreen engine-load test proves every QML file parses and every type it
names resolves. It cannot prove a `Connections` handler is bound to a signal
that exists, or that a delegate reads a key the model supplies — both are
resolved dynamically against objects that test never supplies.
`test_lims_qml_bridges.cpp` covers that from the other side: it asserts
against the real bridges' metaobjects that every signal the QML connects to
exists, and against their real property bags that every key a delegate reads
is present.

### 20. Model coverage is 98.96%, and measuring it found a real defect (§ coverage gate)

`codecov.yml` gains a `lims` component scoped to `examples/lims/src/models/**`
and `examples/lims/include/lims/models/**` — the paths
`examples/IMPLEMENTATION.md` rule 5's bar actually names — with a 97% target,
a margin below the measured 855/864 = 98.96% ceiling.

The remaining nine lines are three defensive guards, each unreachable through
the model's own API rather than merely untested: `alreadyDecided`'s empty-key
guard (validation refuses an empty operation key first), `renderSchemaFor`'s
malformed-JSON arm (its input is `schemaJson<A>()`), and
`GetAnalysisVersion`'s dangling-version arm (a declared foreign key forbids
the row). Each turns an impossible state into a typed error instead of a
dereference, so each is kept.

Measuring rather than assuming was worth it twice. The first measurement came
in at 93.27%, and the gap was not padding: `AnalysisCatalogModel::attachActionLog`
had no caller at all, so the catalogue's entire journaling path was
unverified — and the audit trail's `VerifyResult` branch was **dead**, because
verifications were journaled under an *empty* entity key. That was a real
defect rather than a coverage artifact: a sample's audit trail silently
omitted the second pair of eyes, which is the one thing a four-eyes record
exists to show. Fixed by rekeying the journal to the result's own sample
(same for conflict resolutions), and pinned by its own test.

Neither this component nor rung 5's could have scored anything before this,
because `scripts/coverage.sh`'s rung list had drifted from CMake's and named
only rungs 1–4 — so ledger's carefully-measured 87% gate was matching a set of
files no uploaded report contained. Fixed here.

## Findings raised by this rung

- **[morph#163](https://github.com/LASTRADA-Software/morph/issues/163)
  — `ModelKey` rejects strong id types. Closed upstream.** `BRIDGE_MODEL_KEY`
  routed the key through `keyToString`, whose concept admitted only
  `std::integral` or `std::string`, while `IMPLEMENTATION.md` rule 3 mandates
  a strong id struct for entity identity. ledger and kanban carried the
  identical hand-written workaround; lims was the third, which is the
  rule-of-three trigger. `ModelKey` now admits a strong id wrapping a raw key
  (`WrappedModelKey`), and morph#183 deleted all three rungs' hand-written
  blocks — `SampleModel` keys on `SampleId` itself, and an empty id is refused
  by `keyToString` instead of dereferenced.
- **The round-5 "no `and`/`or`/`not` combinators" claim is stale** (build
  order §5 above, corrected in place). They landed in commit 332f82c (#78)
  and are specified in `docs/spec/forms/forms.md`.
- **[morph#164](https://github.com/LASTRADA-Software/morph/issues/164)
  — a forms schema is a pure function of the compiled action type.**
  **Partly closed upstream.** Per-instance *values* now reach the framework's
  own keys through `morph::forms::InstanceConstraints`: the two analysis
  versions declaring 3 and 1 decimal places serve `"x-decimalPlaces":3` and
  `"x-decimalPlaces":1`, and a reading outside the served specification range
  is flagged (`ResultView::outOfSpec`) instead of being stored with nothing
  recording it. What remains is the structural half — the schema's *shape* is
  still a function of the compiled type, so a rung whose definitions are data
  needs one compiled action per unit family rather than per analysis. That is
  what rung 7's runtime custom fields run into head-on.
- **[morph#174](https://github.com/LASTRADA-Software/morph/issues/174)
  — a journal entry from an older build decodes leniently to defaults, with no
  signal.** `ActionTraits::fromJson` reads with `error_on_unknown_keys = false`,
  so a renamed field is dropped and the new one takes its default; `LogEntry::v`
  versions the *line format*, not the application payload. An audit trail
  reconstructed across such a rename reports the wrong state with full
  confidence. **This rung was named the owner of the ladder's answer and has
  not closed it** — the finding carries the diagnosis, a measured repro, and
  three options with the trade-off that decides between them.
- **A schema's `required` array can silently contradict its own `x-rules`.**
  `schemaJson`'s required-by-default rule put both `value` and `qualifier`
  in `required` while the `exactlyOneOf` entry beside them said at most one
  may be engaged — an unsatisfiable form, and nothing in `morph::forms`
  detects it. The sanctioned escape (`optionalFields`) works and is used
  here, but it is opt-in and invisible: the contradiction compiles, renders,
  and only shows up as "the client's payload is rejected by the server".
  `mergeSchemaExtras` already walks `formRules` when it emits `x-rules`, so
  it has everything it needs either to drop a field named in an
  `exactlyOneOf`/`mutuallyExclusive`/`atLeastOneOf` rule from `required`, or
  to fail loudly. `docs/spec/forms/forms.md` documents the two features in
  separate sections and never mentions their interaction. Guarded here by a
  test asserting `required` is exactly `["analysisVersionId"]`.
- **[morph#175](https://github.com/LASTRADA-Software/morph/issues/175)
  — the three shipped `IOfflineQueue`s disagree about repeated idempotency
  keys.** `InMemoryOfflineQueue` admits the duplicate; `FileOfflineQueue` and
  `SqliteOfflineQueue` dedup; the interface contract says none of them should,
  and the spec's `SqliteOfflineQueue` section claims a parity with
  `InMemoryOfflineQueue` that does not exist. Three-implementation repro in the
  finding.
- **[morph#197](https://github.com/LASTRADA-Software/morph/issues/197)
  — the offline write path has no model-side seam.** Rule 1 says all domain
  logic lives in models; the offline spec says enqueue-on-failure is the
  application's job at the dispatch site; and a disconnected field client has
  no model to put it in anyway. `FieldOutbox` is this rung's app-layer answer,
  and it carries a real invariant (a client's own successive offline edits must
  chain), not glue. **Dispositioned as app-layer by design:** rule 1 names the
  carve-out and `docs/spec/offline/offline.md` records the reasoning and the
  boundary; a framework primitive is reconsidered when a third rung grows its
  own enqueue path.
- **[morph#172](https://github.com/LASTRADA-Software/morph/issues/172)
  — `MORPH_BUILD_OFFLINE_SQLITE=ON` breaks the build on macOS with a non-Apple
  clang.** `FindSQLite3` resolves the SDK's whole `/usr/include`, which is then
  injected as `-isystem` ahead of libc++'s own headers. The repo's own
  `morph_offline_sqlite_tests` target fails identically, which is why the
  durable queue had never been built here before.
- **[morph#176](https://github.com/LASTRADA-Software/morph/issues/176)
  — `x-rules` has one client-side evaluator and no shared corpus.** The spec
  makes client/server parity the reason the vocabulary is closed, but the only
  client-side implementation is JavaScript inside `DynamicForm.qml`, each side
  is tested against its own expectations, and the renderer conformance kit's
  scope note does not list `x-rules` at all. A non-QML client — which is what
  this rung's field devices are — must reimplement the whole vocabulary,
  vacuity asymmetry and fail-closed rule included.
- **[morph#173](https://github.com/LASTRADA-Software/morph/issues/173)
  — a ladder test whose name contains a semicolon never gets its
  `ladder-<rung>` label.** `morph_add_rung`'s re-labelling step iterates
  `IN LISTS`, which splits the name, so `set_tests_properties` applies to
  nothing and nothing warns. Found here the hard way: `ctest -L ladder-lims`
  reported 85 cases while the binary reported 87. Two lims cases were renamed;
  12 pre-existing ones repo-wide are still affected.
- **[morph#199](https://github.com/LASTRADA-Software/morph/issues/199)
  — a `Quantity`'s exact decimal can only be rendered with its unit appended.**
  `morph::units::toString` always concatenates the unit; the decimal-only
  formatter is in `morph::units::detail`, and `std::format("{}", rational)`
  prints the *fraction* (`"12/5"`), not the decimal. A table that places the
  number and the unit separately has to take the suffix back off a string that
  just had it put on.
- **[morph#201](https://github.com/LASTRADA-Software/morph/issues/201)
  — `Model::onBackendChanged()` runs with no session.** The offline spec names
  it as *the* seam for rich replay outcomes, but `switchBackend` posts it onto
  the model's strand where `session::current()` is null, so a replay that must
  know who is replaying cannot use it. Found by driving replay through
  `switchBackend` rather than calling the hook directly — the §7 suite's own
  helper had been supplying a session the framework never does.
- **`scripts/coverage.sh`'s rung list had drifted from CMake's.** ledger and
  lims were both missing, so neither reached any uploaded report — and rung
  5's codecov component, with a target derived from a genuinely careful
  measurement, was scoring files the report did not contain. A component that
  matches nothing does not fail; it reports nothing. Fixed here.
- **Models cannot self-journal without the registry/dispatcher path.**
  `IModelHolder::recordIfAttached` fires only for holder-constructed models,
  so a plain-constructed one — the only kind a unit test has, and what
  `IMPLEMENTATION.md` rule 5 requires the audit trail to be tested through —
  records nothing. `ledger` (three models), `kanban`, and this rung's
  catalogue each hand-rolled the same `attachActionLog`/`logAction` pair;
  this rung shares one copy between its two models
  (`include/lims/core/self_journal.hpp`) rather than adding a sixth. Not
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
