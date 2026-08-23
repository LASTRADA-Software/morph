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
definitions = form schemas, versioned), `WorksheetModel`.

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
  rejected, or migrated — pick one and prove it. Extend to real binary
  skew: build an old client with `MORPH_CLIENT_ONLY` and run it against a
  new server (additive field must work; a renamed field must fail *loudly*,
  not decode a lab result to a default).
- **Self-conflict in the offline chain**: one field client editing the same
  sample twice offline — the second queued update's base version must
  reference the first *queued* update, not the server state, or replay
  flags the client's own second edit as a conflict (ODK hit exactly this).
- **Precision through unit relations — the rule exists; test it, don't
  redesign it** (round-5 correction): conversion carries the dp tag through
  unchanged, the renderer always submits in the canonical unit at the
  schema's `x-decimalPlaces`, and alternative-unit display rounds half-up.
  What to test instead: (a) **retag-vs-round** — `x-decimalPlaces`
  "enforcement" retags the tag without changing the value, so a hand-built
  over-precise payload stores `1.23456` displayed as `1.2` (spec text and
  code disagree; display ≠ stored is disqualifying in a LIMS — this rung
  owns the decision test, review D1); (b) `x-unitAlternatives` lists
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

`x-decimalPlaces` "enforcement" retags a value's precision tag without
changing the value (upstream issue #159), so a hand-built payload of
`1.23456` against a 3-dp analysis would be *stored* as `1.23456` while every
display of it read `1.235`. Storage disagreeing with display is
disqualifying in a LIMS, so this rung takes the third option: reject the
payload. The check is exact and overflow-free — `Rational` keeps
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

### 10. Rendering is version-bound; validation is not (§4, review D4)

`GetAnalysisSchema{versionId}` serves the result-entry form for one version:
the compiled `CaptureConcentration` schema with that version's own
precision, spec range and detection limits merged into the `value` property
as `x-versionDecimalPlaces` / `x-specLow` / `x-specHigh` /
`x-limitOfDetection` / `x-upperDetectionLimit`. Revising an analysis leaves
the old version's served form byte-identical, which is the ODK property the
README asks for.

Everything `morph::forms` itself derives is compiled and therefore identical
for every version: the `required` array, the `x-rules` list, and
`x-decimalPlaces` (from `Quantity<mg_per_L, 3>`'s template parameter). The
per-version precision is served *beside* `x-decimalPlaces` rather than
overwriting it — `x-decimalPlaces` is a contract the framework enforces on
dispatch, so rewriting it would advertise a promise no code keeps. The
disagreement is visible instead of hidden, and every version-specific rule
this rung actually enforces is a hand-written model check reading the
version row.

`GetAnalysisSchema` **refuses** a version whose canonical unit has no
compiled result-entry action, rather than serving the mg/L form for an amps
analysis.

### 11. Offline replay is the model's job; the write path could not be (§7)

The read half uses the seam the framework provides:
`SampleModel::onBackendChanged()` drains the attached `IOfflineQueue`,
classifies each item, and applies or flags it — all inside the model, exactly
as `docs/spec/offline/offline.md`'s "Model `onBackendChanged()` path"
prescribes. Every drained item is `markDone()`d whatever the outcome, because
that path has no retry budget.

The write half could not be: the framework supplies no enqueue-on-failure
seam, and the machine that must make the decision (a disconnected field
client) has no model on it at all. `include/lims/offline/field_outbox.hpp` is
the app-layer answer, and `docs/findings/008` is the finding.

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
(`docs/findings/007`).

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
visibly incomplete. That is `docs/findings/010`, and this rung does **not**
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
framework owns, which is `docs/findings/011`.

## Findings raised by this rung

- **[`docs/findings/005`](../../docs/findings/005-modelkey-rejects-strong-id-types.md)
  — `ModelKey` rejects strong id types.** `BRIDGE_MODEL_KEY` routes the key
  through `keyToString`, whose concept admits only `std::integral` or
  `std::string`, while `IMPLEMENTATION.md` rule 3 mandates a strong id
  struct for entity identity. ledger and kanban already carry the identical
  hand-written workaround; lims is the third, which is the rule-of-three
  trigger.
- **The round-5 "no `and`/`or`/`not` combinators" claim is stale** (build
  order §5 above, corrected in place). They landed in commit 332f82c (#78)
  and are specified in `docs/spec/forms/forms.md`.
- **[`docs/findings/006`](../../docs/findings/006-forms-schema-cannot-carry-per-instance-data.md)
  — a forms schema is a pure function of the compiled action type.**
  Per-instance data cannot reach the keys the framework enforces: two
  analysis versions declaring 3 and 1 decimal places both serve
  `"x-decimalPlaces":3`, and a value outside a served spec range passes
  `validate()` and is stored unflagged, because the rule vocabulary cannot
  name a bound that lives in a database row. Bridges directly into rung 7's
  runtime custom fields, which are planned on the opposite assumption.
- **[`docs/findings/010`](../../docs/findings/010-journal-payload-evolution-decodes-silently-to-defaults.md)
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
- **[`docs/findings/007`](../../docs/findings/007-offline-queue-idempotency-dedup-divergence.md)
  — the three shipped `IOfflineQueue`s disagree about repeated idempotency
  keys.** `InMemoryOfflineQueue` admits the duplicate; `FileOfflineQueue` and
  `SqliteOfflineQueue` dedup; the interface contract says none of them should,
  and the spec's `SqliteOfflineQueue` section claims a parity with
  `InMemoryOfflineQueue` that does not exist. Three-implementation repro in the
  finding.
- **[`docs/findings/008`](../../docs/findings/008-no-model-seam-for-the-offline-write-path.md)
  — the offline write path has no model-side seam.** Rule 1 says all domain
  logic lives in models; the offline spec says enqueue-on-failure is the
  application's job at the dispatch site; and a disconnected field client has
  no model to put it in anyway. `FieldOutbox` is this rung's app-layer answer,
  and it carries a real invariant (a client's own successive offline edits must
  chain), not glue.
- **[`docs/findings/009`](../../docs/findings/009-offline-sqlite-option-breaks-non-apple-clang-on-macos.md)
  — `MORPH_BUILD_OFFLINE_SQLITE=ON` breaks the build on macOS with a non-Apple
  clang.** `FindSQLite3` resolves the SDK's whole `/usr/include`, which is then
  injected as `-isystem` ahead of libc++'s own headers. The repo's own
  `morph_offline_sqlite_tests` target fails identically, which is why the
  durable queue had never been built here before.
- **[`docs/findings/011`](../../docs/findings/011-no-shared-conformance-corpus-for-x-rules.md)
  — `x-rules` has one client-side evaluator and no shared corpus.** The spec
  makes client/server parity the reason the vocabulary is closed, but the only
  client-side implementation is JavaScript inside `DynamicForm.qml`, each side
  is tested against its own expectations, and the renderer conformance kit's
  scope note does not list `x-rules` at all. A non-QML client — which is what
  this rung's field devices are — must reimplement the whole vocabulary,
  vacuity asymmetry and fail-closed rule included.
- **[`docs/findings/012`](../../docs/findings/012-ladder-rung-label-lost-for-test-names-containing-semicolons.md)
  — a ladder test whose name contains a semicolon never gets its
  `ladder-<rung>` label.** `morph_add_rung`'s re-labelling step iterates
  `IN LISTS`, which splits the name, so `set_tests_properties` applies to
  nothing and nothing warns. Found here the hard way: `ctest -L ladder-lims`
  reported 85 cases while the binary reported 87. Two lims cases were renamed;
  12 pre-existing ones repo-wide are still affected.
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
